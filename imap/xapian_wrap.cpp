#include <errno.h>
#include <config.h>
#include <string.h>
#include <sys/types.h>
#include <syslog.h>

#include <fstream>
#include <sstream>
#include <algorithm>
#include <memory>

extern "C" {
#include <assert.h>
#include "libconfig.h"
#include "util.h"
#include "search_engines.h"
#include "search_part.h"
#include "xmalloc.h"
#include "xapian_wrap.h"
#include "charset.h"
#include "ptrarray.h"
#include "parseaddr.h"


/* generated headers are not necessarily in current directory */
#include "imap/imap_err.h"
};

#include <unicode/unistr.h>
#include <unicode/locid.h>

#include <xapian.h>

#ifdef HAVE_CLD2
#include <cld2/public/compact_lang_det.h>
#endif

// from global.h
extern int charset_flags;

#define SLOT_CYRUSID        0
#define SLOT_DOCLANGS       1
#define SLOT_INDEXLEVEL     2
#define SLOT_INDEXVERSION   3

static const unsigned XAPIAN_MAX_TERM_LENGTH = 200; /* in UTF-8 bytes */

/* ====================================================================== */

static void make_cyrusid(struct buf *dst, const struct message_guid *guid, char doctype)
{
    buf_reset(dst);
    buf_putc(dst, '*');
    buf_putc(dst, doctype);
    buf_putc(dst, '*');
    buf_appendcstr(dst, message_guid_encode(guid));
    buf_cstring(dst);
}

/* ====================================================================== */

/*
 * A brief history of Xapian db versions:
 * Version 0: uses STEM_ALL for all terms, term prefixes don't start with 'X'
 * Version 1: term prefixes start with 'X'
 * Version 2: uses STEM_SOME for some terms
 * Version 3: removes all use of STEM_ALL
 * Version 4: indexes headers and bodies in separate documents
 * Version 5: indexes headers and bodies together and stems by language
 * Version 6: stores all detected languages of a document in slot SLOT_DOCLANGS (deprecated)
 * Version 7: indexes new DELIVEREDTO search part
 * Version 8: reintroduces language indexing for non-English text
 * Version 9: introduces index levels as keys to cyrusid metadata
 * Version 10: indexes new PRIORITY search part
 * Version 11: indexes LIST-ID as single value
 * Version 12: indexes email domains as single values. Supports subdomain search.
 * Version 13: indexes content-type and subtype separately
 * Version 14: adds SLOT_INDEXVERSION to documents
 * Version 15: receives indexed header fields and text in original format (rather than search form)
 * Version 16: indexes entire addr-spec as a single value.  Prevents cross-matching localparts and domains
 * Version 17: normalizes text using libicu's NFKC with CaseFolding. Default stemmer keeps using lowercase Cyrus Search Form.
 */
#define XAPIAN_DB_CURRENT_VERSION 17
#define XAPIAN_DB_MIN_SUPPORTED_VERSION 5

static std::set<int> read_db_versions(const Xapian::Database &database)
{
    std::set<int> versions;

    // db_version is a comma-separated list of version numbers
    std::string val = database.get_metadata("cyrus.db_version");
    if (!val.empty()) {
        strarray_t *vstr = strarray_split(val.c_str(), ",", 0);
        for (int i = 0; i < strarray_size(vstr); i++) {
            int version = std::atoi(strarray_nth(vstr, i));
            if (version) versions.insert(version);
        }
        strarray_free(vstr);
    }
    // Up to version 3 this was named stem version.
    val = database.get_metadata("cyrus.stem-version");
    if (!val.empty()) {
        versions.insert(std::stoi(val));
    }

    return versions;
}

static void write_db_versions(Xapian::WritableDatabase &database, const std::set<int> &versions)
{
    std::ostringstream val;
    for (std::set<int>::iterator it = versions.begin(); it != versions.end(); ++it) {
        if (it != versions.begin()) val << ",";
        val << *it;
    }
    database.set_metadata("cyrus.db_version", val.str());
    database.set_metadata("cyrus.stem-version", "");
}

/* ====================================================================== */
#define XAPIAN_LANG_COUNT_KEYPREFIX "lang.count"
#define XAPIAN_LANG_DOC_KEYPREFIX "lang.doc"

static std::string lang_prefix(const std::string& iso_lang, const std::string& prefix)
{
    std::string ustr = prefix + "XI" + iso_lang;
    std::transform(ustr.begin(), ustr.end(), ustr.begin(), ::toupper);
    return ustr;
}

static std::string lang_doc_key(const char *cyrusid)
{
    return std::string{XAPIAN_LANG_DOC_KEYPREFIX "."} + cyrusid;
}

static std::string lang_count_key(const std::string& iso_lang)
{
    return XAPIAN_LANG_COUNT_KEYPREFIX "." + iso_lang;
}

static int calculate_language_counts(const Xapian::Database& db,
                                     std::map<const std::string, unsigned>& lang_counts)
{
    std::set<int> db_versions = read_db_versions(db);

    if (db_versions.lower_bound(8) == db_versions.begin()) {
        // count all indexed body parts
        size_t nparts = 0;
        for (Xapian::TermIterator it = db.metadata_keys_begin("cyrusid.*P*");
                it != db.metadata_keys_end("cyrusid.*P*"); ++it) {
            nparts++;
        }
        // count body parts with language metadata
        const std::string prefix{XAPIAN_LANG_DOC_KEYPREFIX ".*P*"};
        size_t nlangparts = 0;
        for (Xapian::TermIterator it = db.metadata_keys_begin(prefix);
                it != db.metadata_keys_end(prefix); ++it) {
            lang_counts[db.get_metadata(*it)] += 1;
            nlangparts++;
        }
        // English or unknown language body parts have no metadata.
        lang_counts["en"] += nparts - nlangparts;
        // Sanity check data
        if (nparts < nlangparts) {
            return IMAP_IOERROR;
        }
    }

    return 0;
}

static void remove_legacy_metadata(Xapian::WritableDatabase& db)
{
    const std::string prefix{XAPIAN_LANG_DOC_KEYPREFIX "."};
    for (Xapian::TermIterator key = db.metadata_keys_begin(prefix);
            key != db.metadata_keys_end(prefix); ++key) {

        const std::string& val = db.get_metadata(*key);
        // Remove legacy keys and values.
        if ((*key).find('.') != std::string::npos ||
            (!val.empty() && !isalpha(val[0]))) {
            db.set_metadata(*key, "");
        }
    }
    for (Xapian::docid docid = 1; docid <= db.get_lastdocid(); ++docid) {
        try {
            Xapian::Document doc = db.get_document(docid);
            const std::string& val = doc.get_value(SLOT_DOCLANGS);
            // Remove legacy doclang slot values.
            if (!val.empty() && !isalpha(val[0])) {
                doc.remove_value(SLOT_DOCLANGS);
            }
        }
        catch (Xapian::DocNotFoundError e) {
            // ignore
        }
    }
}

static void write_language_counts(Xapian::WritableDatabase& db,
                                  const std::map<const std::string, unsigned>& lang_counts)
{
    for (Xapian::TermIterator it = db.metadata_keys_begin(XAPIAN_LANG_COUNT_KEYPREFIX);
            it != db.metadata_keys_end(XAPIAN_LANG_COUNT_KEYPREFIX); ++it) {
        db.set_metadata(*it, "");
    }
    for (const std::pair<const std::string, unsigned>& it : lang_counts) {
        db.set_metadata(lang_count_key(it.first), std::to_string(it.second));
    }
}

static void read_language_counts(const Xapian::Database& db,
                                 std::map<const std::string, unsigned>& lang_counts)
{
    std::set<int> db_versions = read_db_versions(db);

    if (db_versions.lower_bound(8) == db_versions.begin()) {
        const std::string prefix(XAPIAN_LANG_COUNT_KEYPREFIX ".");
        for (Xapian::TermIterator it = db.metadata_keys_begin(prefix);
                it != db.metadata_keys_end(prefix); ++it) {
            std::string iso_lang = (*it).substr(prefix.length());
            unsigned count = std::stol(db.get_metadata(*it));
            lang_counts[iso_lang] += count;
        }
    }
}

static void parse_doclangs(const std::string& val, std::set<std::string>& doclangs)
{
    size_t base = 0, pos;
    while ((pos = val.find(',', base)) != std::string::npos) {
        doclangs.insert(val.substr(base, pos - base));
        base = pos + 1;
    }
    doclangs.insert(val.substr(base));
}

static std::string format_doclangs(const std::set<std::string>& doclangs)
{
    std::ostringstream val;
    for (std::set<std::string>::iterator it = doclangs.begin(); it != doclangs.end(); ++it) {
        if (it != doclangs.begin()) val << ",";
        val << *it;
    }
    return val.str();
}

static std::string parse_langcode(const char *str)
{
    std::string lstr(str);
    std::transform(lstr.begin(), lstr.end(), lstr.begin(), ::tolower);
    // accept syntax for two and three letter ISO 639 codes
    if (!(isalpha(lstr[0]) && isalpha(lstr[1]) &&
           (lstr[2] == '\0' || (isalpha(lstr[2]) && lstr[3] == '\0')))) {
        return std::string();
    }
    return lstr;
}

// Process-scoped, thread-unsafe cache of stoppers by ISO 639 code.
static std::map<const std::string, std::unique_ptr<Xapian::Stopper>> stoppers;

static const Xapian::Stopper* get_stopper(const std::string& iso)
{
    // Lookup cached entry.
    try {
        return stoppers.at(iso).get();
    } catch (const std::out_of_range&) {};

    // Lookup language name by ISO code.
    icu::Locale loc(iso.c_str());
    if (loc.isBogus()) return NULL;

    // Read stopper file and add to cache.
    const char *swpath = config_getstring(IMAPOPT_SEARCH_STOPWORD_PATH);
    if (!swpath) return NULL;

    std::string lang_name;
    icu::UnicodeString ulang_name;
    loc.getDisplayLanguage(icu::Locale("en"), ulang_name);
    ulang_name.toLower();
    ulang_name.toUTF8String(lang_name);

    // Open stopword file
    // XXX doesn't play nice with WIN32 paths
    std::string fname(std::string(swpath) + "/" + lang_name + ".txt");
    errno = 0;
    std::ifstream inFile (fname);
    if (inFile.fail()) {
        syslog(LOG_DEBUG, "Xapian: could not open stopword file %s: %s",
                fname.c_str(), errno ? strerror(errno) : "unknown error");
        return NULL;
    }

    // Create and store the Xapian stopper
    stoppers[iso].reset(new Xapian::SimpleStopper(
                std::istream_iterator<std::string>(inFile),
                std::istream_iterator<std::string>()));
    return stoppers[iso].get();
}

class CyrusSearchStemmer : public Xapian::StemImplementation
{
    charset_t utf8 {charset_lookupname("utf-8")};
    std::map<const std::string, std::string> cache;
    Xapian::Stem stem {"en"};

    public:
    virtual ~CyrusSearchStemmer() { charset_free(&utf8); }

    virtual std::string operator() (const std::string &word) override {
        // Is this word already in the cache?
        try {
            return cache.at(word);
        } catch (const std::out_of_range&) {}

        // Convert the word to search form
        std::unique_ptr<char, decltype(std::free)*>
            q {charset_convert(word.c_str(), utf8, charset_flags), std::free};
        std::string s = q ? stem(Xapian::Unicode::tolower(q.get())) : stem(word);
        if (s.size() > XAPIAN_MAX_TERM_LENGTH) return std::string{};

        // Store the normalized word in the cache
        return cache[word] = s;
    }

    virtual std::string get_description () const override {
        return "Cyrus";
    }
};


class FrenchContractionStemmer : public Xapian::StemImplementation
{
    Xapian::Stem stem {"fr"};

    public:

    virtual std::string operator() (const std::string &word) override {

        size_t pos = 0;
        switch (word[0]) {
            case 'q':
                if (word.length() <= 3 || word[1] != 'u') {
                    break;
                }
                pos++;
                // fall through
            case 'c':
            case 'd':
            case 'j':
            case 'l':
            case 'm':
            case 'n':
            case 's':
            case 't':
                // APOSTROPHE (U+0027)
                if (word.length() > pos + 2 && word[pos+1] == 0x27) {
                    return stem(word.substr(pos + 2));
                }
                // RIGHT SINGLE QUOTATION MARK (U+2019)
                // FULLWIDTH APOSTROPHE (U+FF07)
                else if (!word.compare(pos + 1, 3, "\xe2\x80\x99") ||
                         !word.compare(pos + 1, 3, "\xef\xbc\x87")) {
                    return stem(word.substr(pos + 4));
                }
                // fall through
        }
        // not a contraction
        return stem(word);
    }

    virtual std::string get_description () const override {
        return "fr-contraction";
    }
};

static Xapian::Stem get_stemmer(const std::string& iso_lang)
{
    return iso_lang == "fr" ?
        Xapian::Stem{new FrenchContractionStemmer} :
        Xapian::Stem{iso_lang};
}

#ifdef HAVE_CLD2
static std::string detect_language(const struct buf *part)
{
    std::string iso_lang;
    bool reliable = false;

    buf_cstring(part);
    CLD2::Language lang = CLD2::DetectLanguage(
        buf_base(part),
        buf_len(part) < INT_MAX ? static_cast<int>(buf_len(part)) : INT_MAX,
        1,
        &reliable);

    if (reliable && lang != CLD2::UNKNOWN_LANGUAGE) {
        std::string code(CLD2::LanguageCode(lang));
        std::transform(code.begin(), code.end(), code.begin(), ::tolower);
        // Map CLD2 special codes to ISO 639.
        if (code == "zh-hant") {
            code = "zh";
        }
        else if (code == "sr-me" ) {
            code = "sr"; // not a political statement!
        }
        else if (code == "xxx") {
            code = "";
        }
        iso_lang = parse_langcode(code.c_str());
    }

    return iso_lang;
}
#endif /* HAVE_CLD2 */

/* ====================================================================== */

static uint8_t better_indexlevel(uint8_t levela, uint8_t levelb)
{
    uint8_t a = levela & ~SEARCH_INDEXLEVEL_PARTIAL;
    uint8_t b = levelb & ~SEARCH_INDEXLEVEL_PARTIAL;
    if (a > b) return levela;
    if (a < b) return levelb;
    return (levela & SEARCH_INDEXLEVEL_PARTIAL) ? levelb : levela;
}

static uint8_t parse_indexlevel(const std::string& s)
{
    uint8_t level = 0;
    if (hex_to_bin(s.c_str(), s.length(), &level) != 1) {
        return 0;
    }
    return level;
}

static std::string format_indexlevel(uint8_t level)
{
    char hex[4];
    bin_to_hex(&level, 1, hex, BH_LOWER);
    return std::string(hex, 2);
}

/* ====================================================================== */

class CyrusMetadataCompactor : public Xapian::Compactor
{
    public:

        CyrusMetadataCompactor() { }

        std::string resolve_duplicate_metadata(const std::string &key,
                                               size_t num_tags,
                                               const std::string tags[])
        {
            if (key.rfind("cyrusid.", 0) == 0) {
                uint8_t indexlevel = parse_indexlevel(tags[0]);
                size_t bestpos = 0;
                for (size_t i = 1; i < num_tags; i++) {
                    uint8_t level = parse_indexlevel(tags[i]);
                    if (better_indexlevel(indexlevel, level) == level) {
                        indexlevel = level;
                        bestpos = i;
                    }
                }
                return tags[bestpos];
            }

            return tags[0];
        }
};


EXPORTED int xapian_compact_dbs(const char *dest, const char **sources)
{
    int r = 0;
    Xapian::Database db;
    const char *thispath = "(unknown path)";

    try {
        std::set<int> db_versions;
        std::map<const std::string, unsigned> lang_counts;
        std::vector<Xapian::Database> subdbs;

        while (*sources) {
            thispath = *sources;
            Xapian::Database subdb(*sources++);
            db.add_database(subdb);
            subdbs.push_back(subdb);

            // Aggregate db versions.
            bool need_metadata = false;
            for (Xapian::docid docid = 1; docid <= subdb.get_lastdocid(); ++docid) {
                try {
                    Xapian::Document doc = subdb.get_document(docid);
                    const std::string& val = doc.get_value(SLOT_INDEXVERSION);
                    if (!val.empty()) {
                        int version = std::atoi(val.c_str());
                        if (version) db_versions.insert(version);
                    }
                    else need_metadata = true;
                }
                catch (Xapian::DocNotFoundError e) {
                    // ignore
                }
            }
            if (need_metadata) {
                /* At least one document didn't have its index version set.
                 * Read the legacy version from the metadata. */
                std::set<int> md_versions = read_db_versions(subdb);
                db_versions.insert(md_versions.begin(), md_versions.lower_bound(14));
            }

            // Aggregate language counts.
            r = calculate_language_counts(subdb, lang_counts);
            if (r) {
                xsyslog(LOG_ERR, "IOERROR: corrupt language metadata",
                                 "path=<%s>", thispath);
                return r;
            }
        }
        thispath = "(unknown path)";

        // Compact database.
        static CyrusMetadataCompactor comp;
        // FULLER because we never write to compression targets again.
        db.compact(dest, Xapian::Compactor::FULLER | Xapian::DBCOMPACT_MULTIPASS, 0, comp);

        Xapian::WritableDatabase newdb(dest);
        write_db_versions(newdb, db_versions);

        // Clean metadata.
        remove_legacy_metadata(newdb);

        // Reset language counts.
        write_language_counts(newdb, lang_counts);
    }
    catch (const Xapian::Error &err) {
        xsyslog(LOG_ERR, "IOERROR: caught exception",
                         "exception=<%s> path=<%s>",
                         err.get_description().c_str(), thispath);
        r = IMAP_IOERROR;
    }

    return r;
}

/* ====================================================================== */

static std::string get_term_prefix(enum search_part partnum)
{
    assert(partnum > SEARCH_PART_ANY && partnum < SEARCH_NUM_PARTS);

    /*
     * We use term prefixes to store terms per search part.
     * In addition, each Xapian document contains a "XE"
     * prefix to indicate its document type, listed in
     * the XAPIAN_WRAP_DOCTYPE definitions. The "XE" prefix
     * MUST not be used for any search part.
     *
     */
    static const char * const term_prefixes[SEARCH_NUM_PARTS] = {
        NULL,                /* ANY */
        "XF",                /* FROM */
        "XT",                /* TO */
        "XC",                /* CC */
        "XB",                /* BCC */
        "XS",                /* SUBJECT */
        "XL",                /* LISTID */
        "XY",                /* TYPE */
        "XH",                /* HEADERS */
        "",                  /* BODY */
        "XO",                /* LOCATION */
        "XA",                /* ATTACHMENTNAME */
        "XAB",               /* ATTACHMENTBODY */
        "XDT",               /* DELIVEREDTO */
        "XI",                /* LANGUAGE */
        "XP",                /* PRIORITY */
        "XMM",               /* MESSAGEID */
        "XMR",               /* REFERENCES */
        "XMI",               /* INREPLYTO */
    };

    return term_prefixes[partnum];
}

static Xapian::TermGenerator::stem_strategy get_stem_strategy(enum search_part partnum)
{
    static Xapian::TermGenerator::stem_strategy stem_strategy[SEARCH_NUM_PARTS] = {
        // Version 2 and higher
        Xapian::TermGenerator::STEM_NONE,  /* ANY */
        Xapian::TermGenerator::STEM_NONE,  /* FROM */
        Xapian::TermGenerator::STEM_NONE,  /* TO */
        Xapian::TermGenerator::STEM_NONE,  /* CC */
        Xapian::TermGenerator::STEM_NONE,  /* BCC */
        Xapian::TermGenerator::STEM_SOME,  /* SUBJECT */
        Xapian::TermGenerator::STEM_NONE,  /* LISTID */
        Xapian::TermGenerator::STEM_NONE,  /* TYPE */
        Xapian::TermGenerator::STEM_NONE,  /* HEADERS */
        Xapian::TermGenerator::STEM_SOME,  /* BODY */
        Xapian::TermGenerator::STEM_SOME,  /* LOCATION */
        Xapian::TermGenerator::STEM_NONE,  /* ATTACHMENTNAME */
        Xapian::TermGenerator::STEM_SOME,  /* ATTACHMENTBODY */
        Xapian::TermGenerator::STEM_NONE,  /* DELIVEREDTO */
        Xapian::TermGenerator::STEM_NONE,  /* LANGUAGE */
        Xapian::TermGenerator::STEM_NONE,  /* PRIORITY */
        Xapian::TermGenerator::STEM_NONE,  /* MESSAGEID */
        Xapian::TermGenerator::STEM_NONE,  /* REFERENCES */
        Xapian::TermGenerator::STEM_NONE,  /* INREPLYTO */
    };

    return stem_strategy[partnum];
}

/* For all db paths in sources that are not using the latest database
 * version or not readable, report their paths in toreindex */
EXPORTED void xapian_check_if_needs_reindex(const strarray_t *sources,
                                            strarray_t *toreindex,
                                            int always_upgrade)
{
    // Check the version of all dbs in sources
    for (int i = 0; i < sources->count; i++) {
        const char *thispath = strarray_nth(sources, i);
        try {
            for (const int& it: read_db_versions(Xapian::Database{thispath})) {
                if (it < XAPIAN_DB_MIN_SUPPORTED_VERSION ||
                        (always_upgrade && (it != XAPIAN_DB_CURRENT_VERSION))) {
                    strarray_add(toreindex, thispath);
                }
            }
        }
        catch (const Xapian::Error &err) {
            strarray_add(toreindex, thispath);
        }
    }
}

/* ====================================================================== */

static inline void add_boolean_nterm(Xapian::Document& doc, const std::string& term)
{
    if (term.size() && term.size() < XAPIAN_MAX_TERM_LENGTH) {
        doc.add_boolean_term(term);
    }
}

struct xapian_dbw
{
    // Database context.
    Xapian::WritableDatabase *database;
    ptrarray_t otherdbs;
    Xapian::TermGenerator *term_generator;
    Xapian::Stem *default_stemmer;
    const Xapian::Stopper* default_stopper;
    bool is_inmemory;
    // Document context.
    Xapian::Document *document;
    char doctype;
    char *cyrusid;
    std::set<std::string> *doclangs;
    std::vector<std::string> *subjects;
};

static Xapian::TermGenerator *new_term_generator(void)
{
    auto termgen = new Xapian::TermGenerator;
    termgen->set_max_word_length(XAPIAN_MAX_TERM_LENGTH);
    /* Always enable CJK word tokenization */
#if defined(USE_XAPIAN_WORD_BREAKS)
    termgen->set_flags(Xapian::TermGenerator::FLAG_WORD_BREAKS,
            ~Xapian::TermGenerator::FLAG_WORD_BREAKS);
#elif defined(USE_XAPIAN_CJK_WORDS)
    termgen->set_flags(Xapian::TermGenerator::FLAG_CJK_WORDS,
            ~Xapian::TermGenerator::FLAG_CJK_WORDS);
#else
    termgen->set_flags(Xapian::TermGenerator::FLAG_CJK_NGRAM,
            ~Xapian::TermGenerator::FLAG_CJK_NGRAM);
#endif
    return termgen;
}


static int xapian_dbw_init(xapian_dbw_t *dbw)
{
    dbw->default_stemmer = new Xapian::Stem(new CyrusSearchStemmer);
    dbw->default_stopper = get_stopper("en");
    dbw->term_generator = new_term_generator();
    dbw->doclangs = new std::set<std::string>;
    dbw->subjects = new std::vector<std::string>;
    return 0;
}

EXPORTED int xapian_dbw_open(const char **paths, xapian_dbw_t **dbwp, int mode)
{
    xapian_dbw_t *dbw = (xapian_dbw_t *)xzmalloc(sizeof(xapian_dbw_t));
    int r = 0;
    const char *thispath = *paths++;

    std::set<int> db_versions;
    try {
        int flags = Xapian::DB_BACKEND_GLASS|Xapian::DB_RETRY_LOCK;
        if (mode & XAPIAN_DBW_NOSYNC) {
            flags |= Xapian::DB_DANGEROUS|Xapian::DB_NO_SYNC;
        }
        try {
            dbw->database = new Xapian::WritableDatabase{thispath, flags|Xapian::DB_OPEN};
            db_versions = read_db_versions(*dbw->database);
        } catch (Xapian::DatabaseOpeningError &e) {
            /* It's OK not to atomically create or open, since we can assume
             * the xapianactive file items to be locked. */
            dbw->database = new Xapian::WritableDatabase{thispath, flags|Xapian::DB_CREATE};
        }
        if (db_versions.find(XAPIAN_DB_CURRENT_VERSION) == db_versions.end()) {
            // Always index using latest database version.
            db_versions.insert(XAPIAN_DB_CURRENT_VERSION);
            write_db_versions(*dbw->database, db_versions);
        }

        r = xapian_dbw_init(dbw);

    }
    catch (const Xapian::DatabaseLockError &err) {
        /* somebody else is already indexing this user.  They may be doing a different
         * mailbox, so we need to re-insert this mailbox into the queue! */
        r = IMAP_MAILBOX_LOCKED;
    }
    catch (const Xapian::Error &err) {
        xsyslog(LOG_ERR, "IOERROR: caught exception",
                         "exception=<%s> path=<%s>",
                         err.get_description().c_str(), thispath);
        r = IMAP_IOERROR;
    }

    if (r) {
        xapian_dbw_close(dbw);
        return r;
    }

    /* open the read-only databases */
    if (mode & XAPIAN_DBW_XAPINDEXED) {
        while (*paths) {
            try {
                thispath = *paths;
                ptrarray_append(&dbw->otherdbs, new Xapian::Database{*paths++});
            }
            catch (const Xapian::Error &err) {
                xsyslog(LOG_ERR, "IOERROR: reading database",
                                 "exception=<%s> path=<%s>",
                                 err.get_description().c_str(), thispath);
            }
        }
    }

    *dbwp = dbw;

    return 0;
}

EXPORTED int xapian_dbw_openmem(struct xapian_dbw **dbwp)
{
    xapian_dbw_t *dbw = (xapian_dbw_t *)xzmalloc(sizeof(xapian_dbw_t));
    dbw->is_inmemory = true;

    dbw->database = new Xapian::WritableDatabase{"", Xapian::DB_BACKEND_INMEMORY};
    write_db_versions(*dbw->database, {XAPIAN_DB_CURRENT_VERSION});

    int r = xapian_dbw_init(dbw);
    if (r) {
        xapian_dbw_close(dbw);
        dbw = NULL;
    }

    *dbwp = dbw;
    return r;
}

EXPORTED void xapian_dbw_close(xapian_dbw_t *dbw)
{
    if (!dbw) return;
    try {
        delete dbw->database;
        delete dbw->term_generator;
        delete dbw->document;
        delete dbw->default_stemmer;
        delete dbw->doclangs;
        delete dbw->subjects;
        for (int i = 0; i < dbw->otherdbs.count; i++) {
            delete (Xapian::Database *)ptrarray_nth(&dbw->otherdbs, i);
        }
        ptrarray_fini(&dbw->otherdbs);
        free(dbw->cyrusid);
        free(dbw);
    }
    catch (const Xapian::Error &err) {
        xsyslog(LOG_ERR, "IOERROR: caught exception",
                         "exception=<%s>",
                         err.get_description().c_str());
    }
}

EXPORTED int xapian_dbw_begin_txn(xapian_dbw_t *dbw)
{
    int r = 0;
    try {
        dbw->database->begin_transaction();
    }
    catch (const Xapian::Error &err) {
        xsyslog(LOG_ERR, "IOERROR: caught exception",
                         "exception=<%s>",
                         err.get_description().c_str());
        r = IMAP_IOERROR;
    }
    return r;
}

EXPORTED int xapian_dbw_commit_txn(xapian_dbw_t *dbw)
{
    int r = 0;
    try {
        dbw->database->commit_transaction();
    }
    catch (const Xapian::Error &err) {
        xsyslog(LOG_ERR, "IOERROR: caught exception",
                         "exception=<%s>",
                         err.get_description().c_str());
        r = IMAP_IOERROR;
    }
    return r;
}

EXPORTED int xapian_dbw_cancel_txn(xapian_dbw_t *dbw)
{
    int r = 0;
    try {
        dbw->database->cancel_transaction();
    }
    catch (const Xapian::Error &err) {
        xsyslog(LOG_ERR, "IOERROR: caught exception",
                         "exception=<%s>",
                         err.get_description().c_str());
        r = IMAP_IOERROR;
    }
    return r;
}

EXPORTED int xapian_dbw_begin_doc(xapian_dbw_t *dbw,
                                  const struct message_guid *guid,
                                  char doctype)
{
    int r = 0;

    try {
        delete dbw->document;
        dbw->document = new Xapian::Document;
        dbw->doctype = doctype;
        /* Set document id and type */
        struct buf buf = BUF_INITIALIZER;
        make_cyrusid(&buf, guid, doctype);
        dbw->document->add_value(SLOT_CYRUSID, buf_cstring(&buf));
        dbw->cyrusid = buf_release(&buf);
        add_boolean_nterm(*dbw->document, std::string("XE") + doctype);
        /* Initialize term generator */
        dbw->term_generator->set_document(*dbw->document);
        dbw->term_generator->set_termpos(1);
    }
    catch (const Xapian::Error &err) {
        xsyslog(LOG_ERR, "IOERROR: caught exception",
                         "exception=<%s>",
                         err.get_description().c_str());
        r = IMAP_IOERROR;
    }
    return r;
}

static int add_language_part(xapian_dbw_t *dbw, const struct buf *part, enum search_part partnum)
{
    std::string prefix(get_term_prefix(partnum));
    std::string val = parse_langcode(buf_cstring(part));
    if (val.empty()) {
        syslog(LOG_INFO, "Xapian: not a valid ISO 639 code: %s",
                buf_cstring(part));
        return 0;
    }
    add_boolean_nterm(*dbw->document, prefix + val);
    return 0;
}

static std::string parse_priority(const char *str)
{
    const char *err;
    uint32_t u;
    if (parseuint32(str, &err, &u) == -1 || *err || u == 0) {
        return std::string();
    }
    return std::to_string(u);
}

static int add_priority_part(xapian_dbw_t *dbw, const struct buf *part, enum search_part partnum)
{
    std::string prefix(get_term_prefix(partnum));
    if (buf_len(part)) {
        std::string val = parse_priority(buf_cstring(part));
        if (val.empty()) {
            syslog(LOG_DEBUG, "Xapian: not a valid priority: %s",
                    buf_cstring(part));
            return 0;
        }
        add_boolean_nterm(*dbw->document, prefix + val);
    }
    return 0;
}

static std::string parse_listid(const char *str)
{
    std::string val;

    /* Extract list-id */
    const char *start = strrchr(str, '<');
    if (start) {
        /* RFC2919 list-id header (with optional closing bracket) */
        const char *end = strchr(++start, '>');
        if (end)
            val = std::string(start, end - start);
        else
            val = std::string(start);
    }
    else {
        /* Groups-style header: 'list list-id[; contact list-contact]'
         * As seen at Google Group, Yahoo, et al. */
        for (start = str; isspace(*start); start++) {}
        if (!strncasecmp("list", start, 4) && isspace(start[4])) {
            for (start = start + 4; isspace(*start); start++) {}
            if (*start) {
                const char *end = strchr(start, ';');
                if (!end || end - start) {
                    val = end ? std::string(start, end - start) : std::string{start};
                }
            }
        }
        /* just raw value, that's OK too, like sentry creates.  Parse up to first whitespace */
        else {
            const char *end;
            for (end = start; *end && !isspace(*end); end++) {}
            val = std::string(start, end - start);
        }
    }

    /* Normalize list-id */
    val.erase(std::remove_if(val.begin(), val.end(), isspace), val.end());
    return val;
}

static int add_listid_part(xapian_dbw_t *dbw, const struct buf *part, enum search_part partnum)
{
    std::string prefix(get_term_prefix(partnum));

    /* Normalize list-id */
    std::string val = parse_listid(buf_cstring(part));
    val.erase(std::remove_if(val.begin(), val.end(), isspace), val.end());
    if (val.empty()) {
        syslog(LOG_DEBUG, "Xapian: not a valid list-id: %s",
                buf_cstring(part));
        return 0;
    }

    add_boolean_nterm(*dbw->document, prefix + val);
    return 0;
}

static int add_email_part(xapian_dbw_t *dbw, const struct buf *part, enum search_part partnum)
{
    std::string prefix(get_term_prefix(partnum));
    struct address_itr itr;
    address_itr_init(&itr, buf_cstring(part), 0);

    const struct address *addr;
    while ((addr = address_itr_next(&itr))) {
        if (addr->invalid) {
            continue;
        }
        if (addr->name) {
            dbw->term_generator->set_stemmer(Xapian::Stem());
            dbw->term_generator->set_stopper(NULL);
            dbw->term_generator->index_text(Xapian::Utf8Iterator(addr->name), 1, prefix + 'N');

            dbw->term_generator->set_stemmer(Xapian::Stem());
            dbw->term_generator->set_stopper(NULL);
            dbw->term_generator->index_text(Xapian::Utf8Iterator(addr->name), 1, prefix);
        }
        if (addr->mailbox) {
            // index mailbox as single value
            std::string val(addr->mailbox);
            // ignore whitespace (as seen in quoted mailboxes)
            val.erase(std::remove_if(val.begin(), val.end(), isspace), val.end());
            add_boolean_nterm(*dbw->document, prefix + 'L' + val);
            // index individual terms
            dbw->term_generator->set_stemmer(Xapian::Stem());
            dbw->term_generator->set_stopper(NULL);
            dbw->term_generator->index_text(Xapian::Utf8Iterator(val), 1, prefix);
        }
        if (addr->domain && strcmp(addr->domain, "unspecified-domain") != 0) {
            char *utf8_domain = charset_idna_to_utf8(addr->domain);
            char *puny_domain = charset_idna_to_ascii(addr->domain);

            const char *domains[3] = {
                addr->domain, utf8_domain, puny_domain
            };

            for (size_t i = 0; i < 3; i++) {
                if (!domains[i] || (i && !strcasecmp(addr->domain, domains[i])))
                    continue;

                struct address myaddr = *addr;
                myaddr.domain = domains[i];

                // index entire addr-spec
                if (char *a = address_get_all(&myaddr, /*canon_domain*/1)) {
                    add_boolean_nterm(*dbw->document, prefix + 'A' + a);
                    free(a);
                }

                // index reversed domain
                std::string revdomain;
                strarray_t *sa = strarray_split(myaddr.domain, ".", 0);
                for (int i = strarray_size(sa) - 1; i >= 0; i--) {
                    revdomain.append(strarray_nth(sa, i));
                    if (i > 0) {
                        revdomain.append(1, '.');
                    }
                }
                strarray_free(sa);
                add_boolean_nterm(*dbw->document, prefix + "D" + revdomain);

                // index individual terms
                dbw->term_generator->set_stemmer(Xapian::Stem());
                dbw->term_generator->set_stopper(NULL);
                dbw->term_generator->index_text(
                    Xapian::Utf8Iterator(myaddr.domain), 1, prefix);
            }

            free(utf8_domain);
            free(puny_domain);
        }
    }

    address_itr_fini(&itr);
    return 0;
}

static std::pair<std::string, std::string> parse_content_type(const char *str)
{
    std::pair<std::string, std::string> ret;
    struct buf buf = BUF_INITIALIZER;

    const char *sep = strchr(str, '/');
    if (sep) {
        /* type */
        buf_setmap(&buf, str, sep - str);
        buf_lcase(&buf);
        buf_trim(&buf);
        ret.first = std::string(buf_cstring(&buf));
        /* subtype */
        buf_setcstr(&buf, sep + 1);
        buf_lcase(&buf);
        buf_trim(&buf);
        ret.second = std::string(buf_cstring(&buf));
    }
    else {
        /* type or subtype */
        buf_setcstr(&buf, str);
        buf_lcase(&buf);
        buf_trim(&buf);
        ret.first = std::string(buf_cstring(&buf));
    }

    buf_free(&buf);
    return ret;
}

static int add_type_part(xapian_dbw_t *dbw, const struct buf *part, enum search_part partnum)
{
    std::string prefix(get_term_prefix(partnum));
    std::pair<std::string, std::string> ct = parse_content_type(buf_cstring(part));
    if (!ct.first.empty()) {
        add_boolean_nterm(*dbw->document, prefix + "T" + ct.first);
    }
    if (!ct.second.empty()) {
        add_boolean_nterm(*dbw->document, prefix + "S" + ct.second);
    }
    if (!ct.first.empty() && !ct.second.empty()) {
        add_boolean_nterm(*dbw->document, prefix + ct.first + '/' + ct.second);
    }
    return 0;
}

static int add_msgid_part(xapian_dbw_t *dbw, const struct buf *part, enum search_part partnum)
{
    std::string prefix(get_term_prefix(partnum));
    add_boolean_nterm(*dbw->document, prefix + buf_cstring(part));
    return 0;
}

static int add_text_part(xapian_dbw_t *dbw, const struct buf *part, enum search_part partnum)
{
    std::string prefix(get_term_prefix(partnum));
    int r = 0;

    // Index text.
    Xapian::TermGenerator::stem_strategy stem_strategy =
        get_stem_strategy(partnum);
    dbw->term_generator->set_stemming_strategy(stem_strategy);

    if (stem_strategy != Xapian::TermGenerator::STEM_NONE) {
        if (config_getswitch(IMAPOPT_SEARCH_INDEX_LANGUAGE)){
            // Index by language.
#ifndef HAVE_CLD2
            // XXX is this really an "IOERROR"?
            xsyslog(LOG_ERR, "IOERROR: language indexing requires CLD2 library",
                             NULL);
            return IMAP_IOERROR;
#else

            if (search_part_is_body(partnum)) {
                const std::string iso_lang = detect_language(part);
                if (!iso_lang.empty()) {
                    if (iso_lang.compare("en")) {
                        // Stem and index by non-default language.
                        try {
                            dbw->term_generator->set_stemmer(get_stemmer(iso_lang));
                            dbw->term_generator->set_stopper(get_stopper(iso_lang));
                            dbw->term_generator->index_text(Xapian::Utf8Iterator(part->s, part->len),
                                    1, lang_prefix(iso_lang, prefix));
                        } catch (const Xapian::InvalidArgumentError &err) {
                            syslog(LOG_DEBUG, "Xapian: no stemmer for language %s",
                                    iso_lang.c_str());
                        }
                    }
                    if (dbw->doctype == 'P') {
                        // Keep track of stemmer language.
                        std::string key = lang_doc_key(dbw->cyrusid);
                        dbw->database->set_metadata(key, iso_lang);
                        dbw->document->add_value(SLOT_DOCLANGS, iso_lang);
                        // Update language counts for body parts.
                        key = lang_count_key(iso_lang);
                        const std::string val = dbw->database->get_metadata(key);
                        dbw->database->set_metadata(key, val.empty() ?
                                "1" : std::to_string(std::stoi(val) + 1));
                    }
                    // Store detected languages in document.
                    dbw->doclangs->insert(iso_lang.c_str());
                    add_boolean_nterm(*dbw->document, "XI" + iso_lang);
                }
            }
            else if (partnum == SEARCH_PART_SUBJECT) {
                // Keep subject text to index by language later.
                dbw->subjects->push_back(buf_cstring(part));
            }
#endif /* HAVE_CLD2 */
        }

        // Index with default stemmer.
        dbw->term_generator->set_stemmer(*dbw->default_stemmer);
        dbw->term_generator->set_stopper(dbw->default_stopper);
    } else {
        // Index with no stemming.
        dbw->term_generator->set_stemmer(Xapian::Stem());
        dbw->term_generator->set_stopper(NULL);
    }
    dbw->term_generator->index_text(Xapian::Utf8Iterator(part->s, part->len), 1, prefix);

    return r;
}

EXPORTED int xapian_dbw_doc_part(xapian_dbw_t *dbw,
                                 const struct buf *part,
                                 enum search_part partnum)
{
    int r = 0;

    try {
        // Handle search parts.
        switch (partnum) {
            case SEARCH_PART_PRIORITY:
                r = add_priority_part(dbw, part, partnum);
                break;
            case SEARCH_PART_LISTID:
                r = add_listid_part(dbw, part, partnum);
                break;
            case SEARCH_PART_LANGUAGE:
                r = add_language_part(dbw, part, partnum);
                break;
            case SEARCH_PART_FROM:
            case SEARCH_PART_TO:
            case SEARCH_PART_CC:
            case SEARCH_PART_BCC:
            case SEARCH_PART_DELIVEREDTO:
                r = add_email_part(dbw, part, partnum);
                break;
            case SEARCH_PART_TYPE:
                r = add_type_part(dbw, part, partnum);
                break;
            case SEARCH_PART_INREPLYTO:
            case SEARCH_PART_MESSAGEID:
            case SEARCH_PART_REFERENCES:
                r = add_msgid_part(dbw, part, partnum);
                break;
            default:
                r = add_text_part(dbw, part, partnum);
        }
        // Finalize index.
        dbw->term_generator->increase_termpos();
    }
    catch (const Xapian::Error &err) {
        xsyslog(LOG_ERR, "IOERROR: caught exception",
                         "exception=<%s>",
                         err.get_description().c_str());
        r = IMAP_IOERROR;
    }
    return r;
}

EXPORTED int xapian_dbw_end_doc(xapian_dbw_t *dbw, uint8_t indexlevel)
{
    int r = 0;

    assert(indexlevel > 0);

    try {
        if (config_getswitch(IMAPOPT_SEARCH_INDEX_LANGUAGE)){
            // Keep track of languages used in this message.
            if (dbw->doctype == 'G') {
                std::string val = format_doclangs(*dbw->doclangs);
                dbw->database->set_metadata(lang_doc_key(dbw->cyrusid), val);
                dbw->document->add_value(SLOT_DOCLANGS, val);
            }
            // Index subjects by detected document languages.
            for (std::set<std::string>::iterator it = dbw->doclangs->begin(); it != dbw->doclangs->end(); ++it) {
                std::string iso_lang = *it;
                if (iso_lang.compare("en")) {
                    try {
                        std::string prefix = lang_prefix(
                            iso_lang, get_term_prefix(SEARCH_PART_SUBJECT));
                        dbw->term_generator->set_stemmer(get_stemmer(iso_lang));
                        dbw->term_generator->set_stopper(get_stopper(iso_lang));
                        for (const std::string& subject : *dbw->subjects)
                            dbw->term_generator->index_text(Xapian::Utf8Iterator(subject), 1, prefix);
                    } catch (const Xapian::InvalidArgumentError &err) {
                        // ignore unknown stemmer
                    }
                }
            }
        }
        dbw->document->add_value(SLOT_INDEXLEVEL, format_indexlevel(indexlevel));
        dbw->document->add_value(SLOT_INDEXVERSION,
                std::to_string(XAPIAN_DB_CURRENT_VERSION));
        dbw->database->add_document(*dbw->document);
        dbw->database->set_metadata("cyrusid." + std::string(dbw->cyrusid),
                                    format_indexlevel(indexlevel));
        delete dbw->document;
        dbw->document = 0;
        dbw->doctype = 0;
        free(dbw->cyrusid);
        dbw->cyrusid = NULL;
        dbw->doclangs->clear();
        dbw->subjects->clear();
    }
    catch (const Xapian::Error &err) {
        xsyslog(LOG_ERR, "IOERROR: caught exception",
                         "exception=<%s>",
                         err.get_description().c_str());
        r = IMAP_IOERROR;
    }
    return r;
}

EXPORTED unsigned long xapian_dbw_total_length(xapian_dbw_t *dbw)
{
    unsigned long res = 0;
    try {
        res = dbw->database->get_total_length();
    }
    catch (const Xapian::Error &err) {
        xsyslog(LOG_ERR, "IOERROR: caught exception",
                         "exception=<%s>",
                         err.get_description().c_str());
    }
    return res;
}

EXPORTED uint8_t xapian_dbw_is_indexed(xapian_dbw_t *dbw,
                                       const struct message_guid *guid,
                                       char doctype)
{
    struct buf buf = BUF_INITIALIZER;
    make_cyrusid(&buf, guid, doctype);
    std::string key = "cyrusid." + std::string(buf_cstring(&buf));
    buf_free(&buf);

    /* indexed in the current DB? */
    uint8_t indexlevel = parse_indexlevel(dbw->database->get_metadata(key));
    if (indexlevel == SEARCH_INDEXLEVEL_BEST ||
            (indexlevel && doctype == XAPIAN_WRAP_DOCTYPE_PART)) {
        return indexlevel;
    }

    /* indexed in other DBs? */
    for (int i = 0; i < dbw->otherdbs.count; i++) {
        Xapian::Database *database = (Xapian::Database *)ptrarray_nth(&dbw->otherdbs, i);
        uint8_t level = parse_indexlevel(database->get_metadata(key));
        if (level == SEARCH_INDEXLEVEL_BEST ||
                (level && doctype == XAPIAN_WRAP_DOCTYPE_PART)) {
            return level;
        }
        else indexlevel = better_indexlevel(indexlevel, level);
    }

    return indexlevel;
}

/* ====================================================================== */

struct xapian_db
{
    std::string *paths;
    Xapian::Database *database; // all but version 4 databases
    std::vector<Xapian::Database> *subdbs; // all database subdbs
    Xapian::Stem *default_stemmer;
    const Xapian::Stopper* default_stopper;
    std::set<std::string> *stem_languages;
    Xapian::QueryParser *parser;
    std::set<unsigned> *db_versions;
    xapian_dbw_t *dbw;
};

static int xapian_db_init(xapian_db_t *db, int is_inmemory)
{
    int r = 0;

    try {
        db->parser = new Xapian::QueryParser;
        db->parser->set_default_op(Xapian::Query::OP_AND);
        db->default_stemmer = new Xapian::Stem(new CyrusSearchStemmer);
        db->default_stopper = get_stopper("en");
        db->stem_languages = new std::set<std::string>;

        if (!is_inmemory) {
            // Determine stemmer languages (in addition to English).
            std::map<const std::string, unsigned> lang_counts;
            unsigned total_doccount = 0;
            for (const Xapian::Database& subdb : *db->subdbs) {
                read_language_counts(subdb, lang_counts);
                total_doccount += subdb.get_doccount();
            }
            total_doccount /= 2; // Crude estimate.
            for (std::pair<const std::string, unsigned>& it : lang_counts) {
                if (it.first.compare("en") && ((double) it.second / total_doccount) >= 0.05) {
                    db->stem_languages->insert(it.first);
                }
            }
        }
    }
    catch (const Xapian::Error &err) {
        xsyslog(LOG_ERR, "IOERROR: caught exception",
                         "exception=<%s>",
                         err.get_description().c_str());
        r = IMAP_IOERROR;
    }

    return r;
}

EXPORTED int xapian_db_open(const char **paths, xapian_db_t **dbp)
{
    xapian_db_t *db = (xapian_db_t *)xzmalloc(sizeof(xapian_db_t));
    const char *thispath = "(unknown)";
    int r = 0;

    try {
        db->paths = new std::string;
        while (paths && *paths) {
            thispath = *paths++;
            Xapian::Database subdb {thispath};
            std::set<int> db_versions = read_db_versions(subdb);
            if (db_versions.empty()) {
                syslog(LOG_ERR, "xapian_wrapper: invalid db version in %s", thispath);
                r = IMAP_INTERNAL;
                goto done;
            }
            if (!db->db_versions)
                db->db_versions = new std::set<unsigned>;
            db->db_versions->insert(db_versions.begin(), db_versions.end());
            // Check for experimental v4 indexes, they were bogus.
            if (db_versions.find(4) != db_versions.end()) {
                xsyslog(LOG_WARNING, "deprecated v4 index detected, "
                        "search may return wrong results",
                        "db=<%s>", thispath);
            }
            // Add database.
            if (!db->database) db->database = new Xapian::Database;
            db->database->add_database(subdb);

            // Xapian database has no API to access subdbs.
            if (!db->subdbs) db->subdbs = new std::vector<Xapian::Database>;
            db->subdbs->push_back(subdb);

            db->paths->append(thispath).push_back(' ');
        }
        thispath = "(unknown)";

        if (!db->database) {
            r = IMAP_NOTFOUND;
            goto done;
        }

        r = xapian_db_init(db, 0);
        if (r) goto done;
    }
    catch (const Xapian::Error &err) {
        xsyslog(LOG_ERR, "IOERROR: caught exception",
                         "exception=<%s> path=<%s>",
                         err.get_description().c_str(), thispath);
        r = IMAP_IOERROR;
    }

done:
    if (r)
        xapian_db_close(db);
    else
        *dbp = db;

    return r;
}

EXPORTED int xapian_db_opendbw(struct xapian_dbw *dbw, xapian_db_t **dbp)
{
    xapian_db_t *db = (xapian_db_t *)xzmalloc(sizeof(xapian_db_t));

    db->dbw = dbw;
    db->database = dbw->database;
    db->db_versions = new std::set<unsigned>();
    std::set<int> dbw_versions = read_db_versions(*dbw->database);
    db->db_versions->insert(dbw_versions.begin(), dbw_versions.end());
    db->subdbs = new std::vector<Xapian::Database>;
    db->subdbs->push_back(*dbw->database);

    int r = xapian_db_init(db, dbw->is_inmemory);
    if (r) {
        xapian_db_close(db);
        db = NULL;
    }

    *dbp = db;
    return r;
}

EXPORTED void xapian_db_close(xapian_db_t *db)
{
    if (!db) return;
    try {
        if (!db->dbw) delete db->database;
        delete db->parser;
        delete db->paths;
        delete db->db_versions;
        delete db->default_stemmer;
        delete db->stem_languages;
        delete db->subdbs;
        free(db);
    }
    catch (const Xapian::Error &err) {
        /* XXX - memory leak? */
        xsyslog(LOG_ERR, "IOERROR: caught exception",
                         "exception=<%s>",
                         err.get_description().c_str());
    }
}

EXPORTED unsigned xapian_db_min_index_version(xapian_db_t *db)
{
    auto it = db->db_versions->begin();
    return it != db->db_versions->end() ? *it : XAPIAN_DB_CURRENT_VERSION;
}

EXPORTED int xapian_db_langstats(xapian_db_t *db, ptrarray_t* lstats,
                                 size_t *nolang)
{
    std::map<const std::string, unsigned> lang_counts;
    size_t total_part = 0;
    size_t total_lang = 0;

    for (const Xapian::Database& subdb : *db->subdbs) {
        // count body parts
        for (Xapian::TermIterator it = subdb.metadata_keys_begin("cyrusid.*P*");
                it != subdb.metadata_keys_end("cyrusid.*P*"); ++it) {
            total_part++;
        }
        // cummulate language counts
        read_language_counts(subdb, lang_counts);
    }
    for (const std::pair<const std::string, unsigned>& counts : lang_counts) {
        struct search_langstat *stat = (struct search_langstat*)
                                       xzmalloc(sizeof(struct search_langstat));
        stat->iso_lang = xstrdup(counts.first.c_str());
        stat->count = counts.second;
        ptrarray_append(lstats, stat);
        total_lang += counts.second;
    }
    *nolang = total_part > total_lang ? total_part - total_lang : 0;

    return 0;
}

EXPORTED void xapian_query_add_stemmer(xapian_db_t *db, const char *iso_lang)
{
    if (strcmp(iso_lang, "en") != 0)
        db->stem_languages->insert(iso_lang);
}

static Xapian::Query* query_new_textmatch(const xapian_db_t *db,
                                          enum search_part partnum,
                                          const char *match,
                                          Xapian::TermGenerator::stem_strategy tg_stem_strategy)
{
    unsigned flags = Xapian::QueryParser::FLAG_PHRASE |
                     Xapian::QueryParser::FLAG_WILDCARD;
    std::string prefix(get_term_prefix(partnum));

    std::string lmatch = Xapian::Unicode::tolower(match);

    if (tg_stem_strategy != Xapian::TermGenerator::STEM_NONE) {

        // Query without any stemmer.
        db->parser->set_stemmer(Xapian::Stem());
        db->parser->set_stopper(NULL);
        db->parser->set_stemming_strategy(Xapian::QueryParser::STEM_NONE);
        Xapian::Query q = db->parser->parse_query(lmatch, flags, prefix);

        // Query with default stemmer. But don't stem stopwords.
        if (!db->default_stopper || !(*db->default_stopper)(lmatch)) {
            db->parser->set_stemmer(*db->default_stemmer);
            db->parser->set_stopper(db->default_stopper);
            db->parser->set_stemming_strategy(Xapian::QueryParser::STEM_SOME);
            q |= db->parser->parse_query(lmatch, flags, prefix);
        }

        // Stem query for each language detected in the index.
        for (const std::string& iso_lang : *db->stem_languages) {
            try {
                const Xapian::Stopper *stopper = get_stopper(iso_lang);
                db->parser->set_stemmer(get_stemmer(iso_lang));
                db->parser->set_stopper(stopper);
                db->parser->set_stemming_strategy(Xapian::QueryParser::STEM_SOME);
                if (!stopper || !(*stopper)(lmatch)) {
                    q |= db->parser->parse_query(lmatch, flags, lang_prefix(iso_lang, prefix));
                }
            } catch (const Xapian::InvalidArgumentError &err) {
                syslog(LOG_INFO, "Xapian: no stemmer for language %s", iso_lang.c_str());
            }
        }

        return new Xapian::Query(q);
    }
    else {
        db->parser->set_stemmer(Xapian::Stem());
        db->parser->set_stopper(NULL);
        db->parser->set_stemming_strategy(Xapian::QueryParser::STEM_NONE);
        return new Xapian::Query {db->parser->parse_query(lmatch, flags, prefix)};
    }
}

static Xapian::Query *query_new_language(const xapian_db_t *db __attribute__((unused)),
                                         enum search_part partnum,
                                         const char *str)
{
    std::string val = parse_langcode(str);
    if (val.empty()) {
        syslog(LOG_DEBUG, "Xapian: invalid language in query: %s", str);
        return new Xapian::Query(Xapian::Query::MatchNothing);
    }
    return new Xapian::Query(get_term_prefix(partnum) + val);
}

static Xapian::Query *query_new_priority(const xapian_db_t *db __attribute__((unused)),
                                         enum search_part partnum,
                                         const char *str)
{
    std::string val = parse_priority(str);
    if (val.empty()) {
        syslog(LOG_DEBUG, "Xapian: invalid priority in query: %s", str);
        return new Xapian::Query(Xapian::Query::MatchNothing);
    }
    return new Xapian::Query(get_term_prefix(partnum) + val);
}

static Xapian::Query *query_new_listid(const xapian_db_t *db,
                                       enum search_part partnum,
                                       const char *str)
{
    Xapian::Query *q = NULL;
    std::string prefix(get_term_prefix(partnum));

    std::string val = parse_listid(str);
    if (!val.empty()) {
        q = new Xapian::Query(prefix + val);
    }
    else {
        syslog(LOG_DEBUG, "Xapian: invalid listid in query: %s", str);
        q = new Xapian::Query(Xapian::Query::MatchNothing);
    }

    if (db->db_versions->lower_bound(11) != db->db_versions->begin()) {
        // query in legacy format
        db->parser->set_stemmer(Xapian::Stem());
        db->parser->set_stopper(NULL);
        db->parser->set_stemming_strategy(Xapian::QueryParser::STEM_NONE);
        q = new Xapian::Query(Xapian::Query::OP_OR, *q,
                db->parser->parse_query(str, 0, prefix));
    }

    return q;
}

static Xapian::Query *query_new_messageid(const xapian_db_t *db __attribute__((unused)),
                                          enum search_part partnum,
                                          const char *str)
{
    return new Xapian::Query(get_term_prefix(partnum) + str);
}

static Xapian::Query *query_new_email(const xapian_db_t *db,
                                      enum search_part partnum,
                                      const char *searchstr)
{
    std::string prefix(get_term_prefix(partnum));
    bool is_phrase_query = false;

    unsigned queryflags =
        Xapian::QueryParser::FLAG_PHRASE | Xapian::QueryParser::FLAG_WILDCARD;

    db->parser->set_stemmer(Xapian::Stem());
    db->parser->set_stopper(NULL);
    db->parser->set_stemming_strategy(Xapian::QueryParser::STEM_NONE);

    std::string str = Xapian::Unicode::tolower(searchstr);
    // Remove enclosing phrase search quotes, if any.
    if (str.size() >= 2 && str.front() == '"' && str.back() == '"') {
        str.erase(str.end() - 1);
        str.erase(str.begin());
        if (str.empty()) return NULL;
        is_phrase_query = true;
    }

    size_t atsign_pos = str.find('@');

    if (atsign_pos == std::string::npos || atsign_pos == str.length() - 1) {
        // query free text only
        return new Xapian::Query{
            db->parser->parse_query(searchstr, queryflags, prefix)};
    }

    // Transform query string into an email address.
    std::string email;

    // Form local part, optionally preceeded by name.
    bool have_localpart = false;
    bool wildcard_localpart = false;

    if (atsign_pos) {
        email.append(str, 0, atsign_pos);
    }

    if (!is_phrase_query &&
            !email.empty() && email[email.length() - 1] == '*') {
        wildcard_localpart = true;
        email.erase(email.length() - 1, 1);
    }

    if (!email.empty() && email[email.length() - 1] != '<') {
        have_localpart = true;
    }
    else {
        if (!email.empty())
            email.append(1, '<');
        email.append("xlocalpart");
    }

    email.append(1, '@');

    // Form domain part.
    bool have_domain = false;
    bool wildcard_domain = false;
    bool subdomain_only = false;
    std::string domain(str, atsign_pos + 1);

    if (!is_phrase_query && domain.length() && domain[0] == '*') {
        wildcard_domain = true;
        domain.erase(0, 1);
        if (domain.length() && domain[0] == '.') {
            subdomain_only = true;
            domain.erase(0, 1);
        }
    }

    if (domain.length() && domain[0] != '>') {
        have_domain = true;
        email.append(domain);
    }
    else {
        email.append("xdomain");
        if (domain.length())
            email.append(1, '>');
    }

    // Parse email address.
    struct address *addr = NULL;
    parseaddr_list(email.c_str(), &addr);

    if (!addr) {
        Xapian::Query q = Xapian::Query::MatchNothing;
        if (db->db_versions->lower_bound(12) != db->db_versions->begin()) {
            // query in legacy format
            q |= db->parser->parse_query(searchstr, queryflags, prefix);
        }
        return new Xapian::Query(q);
    }

    // Build query
    if (!have_localpart)
        addr->mailbox = NULL;

    if (!have_domain)
        addr->domain = NULL;

    // Build query.
    std::vector<Xapian::Query> queries;

    if (addr->name) {
        queries.push_back(
                db->parser->parse_query(addr->name, queryflags, prefix + 'N'));
    }

    if (addr->domain) {
        char *utf8_domain = charset_idna_to_utf8(addr->domain);
        char *puny_domain = charset_idna_to_ascii(addr->domain);

        const char *domains[3] = {addr->domain, utf8_domain, puny_domain};

        std::vector<Xapian::Query> domain_queries;

        for (size_t i = 0; i < 3; i++) {
            if (!domains[i] || (i && !strcasecmp(addr->domain, domains[i])))
                continue;

            struct address myaddr = *addr;
            myaddr.domain = domains[i];

            if (myaddr.mailbox && !wildcard_localpart && !wildcard_domain) {
                // Query complete email addresses with A prefix
                Xapian::Query q = Xapian::Query::MatchNothing;
                if (char *a = address_get_all(&myaddr, /*canon_domain*/ 1)) {
                    q = Xapian::Query(prefix + 'A' + a);
                    free(a);
                }

                if (db->db_versions->lower_bound(16) !=
                        db->db_versions->begin()) {
                    // Database version 15 did not index complete addresses
                    // with A prefix, but rather indexed localpart and domain
                    // separately, using L and D prefixes.
                    // This is buggy, as a query for 'foo@bar.com' would
                    // return a message if it has at least one recipient
                    // with localpart 'foo' and another recipient has
                    // domain 'bar.com'.
                    q = Xapian::Query(
                            Xapian::Query::OP_OR, q,
                            Xapian::Query(
                                Xapian::Query::OP_AND,
                                Xapian::Query(
                                    Xapian::Query::OP_VALUE_LE,
                                    Xapian::valueno(SLOT_INDEXVERSION),
                                    std::string("15")),
                                Xapian::Query(Xapian::Query::OP_AND,
                                    Xapian::Query(prefix + 'L' +
                                        myaddr.mailbox),
                                    Xapian::Query(prefix + 'D' +
                                        myaddr.mailbox))));
                }

                domain_queries.push_back(q);
            }
            else {
                // Query domain-only and wildcard domains using D prefix.
                std::string revdomain;
                strarray_t *sa = strarray_split(myaddr.domain, ".", 0);
                for (int i = strarray_size(sa) - 1; i >= 0; i--) {
                    revdomain.append(strarray_nth(sa, i));
                    if (i > 0) {
                        revdomain.append(1, '.');
                    }
                }
                strarray_free(sa);
                if (revdomain.length() && subdomain_only) {
                    revdomain.append(1, '.');
                }
                if (!revdomain.empty()) {
                    std::string term(prefix + 'D' + revdomain);
                    Xapian::Query q =
                        wildcard_domain
                        ? Xapian::Query(Xapian::Query::OP_WILDCARD,
                                term)
                        : Xapian::Query(term);
                    domain_queries.push_back(q);
                }
            }
        }

        if (!domain_queries.empty()) {
            queries.push_back(Xapian::Query(Xapian::Query::OP_OR,
                        domain_queries.begin(),
                        domain_queries.end()));
        }

        free(utf8_domain);
        free(puny_domain);

    }

    if (addr->mailbox && (!addr->domain || wildcard_domain || wildcard_localpart)) {
        // If we can't query with the A prefix, then query with L prefix.
        std::string term(prefix + 'L' + addr->mailbox);
        queries.push_back(
                wildcard_localpart
                ? Xapian::Query(Xapian::Query::OP_WILDCARD, term)
                : Xapian::Query(term));
    }

    // Build return value.
    Xapian::Query q = Xapian::Query::MatchNothing;

    if (!queries.empty()) {
        q |= Xapian::Query(Xapian::Query::OP_AND, queries.begin(),
                queries.end());
    }

    if (db->db_versions->lower_bound(12) != db->db_versions->begin()) {
        // query in legacy format as well
        q |= db->parser->parse_query(searchstr, queryflags, prefix);
    }

    parseaddr_free(addr);
    return new Xapian::Query(q);
}

static void append_alnum(struct buf *buf, const char *ss)
{
    const unsigned char *s = (const unsigned char *)ss;

    for ( ; *s ; ++s) {
        if (Uisalnum(*s))
            buf_putc(buf, *s);
    }
}

static Xapian::Query *query_new_type(const xapian_db_t *db __attribute__((unused)),
                                     enum search_part partnum,
                                     const char *str)
{

    std::pair<std::string, std::string> ct = parse_content_type(str);
    std::string prefix(get_term_prefix(partnum));
    Xapian::Query q = Xapian::Query::MatchNothing;

    bool query_legacy = db->db_versions->lower_bound(13) != db->db_versions->begin();
    struct buf buf = BUF_INITIALIZER;
    unsigned qpflags = Xapian::QueryParser::FLAG_PHRASE |
                       Xapian::QueryParser::FLAG_WILDCARD;

    if (!ct.first.empty() && ct.second.empty()) {
        /* Match either type or subtype */
        if (ct.first != "*") {
            q = Xapian::Query(Xapian::Query::OP_OR,
                    Xapian::Query(prefix + 'T' + ct.first),
                    Xapian::Query(prefix + 'S' + ct.first));
            if (query_legacy) {
                append_alnum(&buf, ct.first.c_str());
                q |= db->parser->parse_query(buf_cstring(&buf), qpflags, prefix);
            }
        }
    }
    else if (ct.first == "*" || ct.second == "*") {
        /* Wildcard query */
        if (!ct.first.empty() && ct.first != "*") {
            /* Match type */
            q = Xapian::Query(prefix + 'T' + ct.first);
            if (query_legacy) {
                append_alnum(&buf, ct.first.c_str());
                q |= db->parser->parse_query(buf_cstring(&buf), qpflags, prefix);
            }
        }
        if (!ct.second.empty() && ct.second != "*") {
            /* Match subtype */
            q = Xapian::Query(prefix + 'S' + ct.second);
            if (query_legacy) {
                append_alnum(&buf, ct.second.c_str());
                q |= db->parser->parse_query(buf_cstring(&buf), qpflags, prefix);
            }
        }
    }
    else if (!ct.first.empty() && !ct.second.empty()) {
        /* Verbatim search */
        q = Xapian::Query(prefix + ct.first + '/' + ct.second);
        if (query_legacy) {
            append_alnum(&buf, ct.first.c_str());
            buf_putc(&buf, '_');
            append_alnum(&buf, ct.second.c_str());
            q |= db->parser->parse_query(buf_cstring(&buf), qpflags, prefix);
        }
    }

    buf_free(&buf);
    return new Xapian::Query(q);
}

static Xapian::Query *xapian_query_new_match_word_break(const xapian_db_t *db,
                                                        enum search_part partnum,
                                                        const char *str)
{
    std::string prefix(get_term_prefix(partnum));

    Xapian::Query *q = new Xapian::Query {db->parser->parse_query(
            str,
#if defined(USE_XAPIAN_WORD_BREAKS)
            Xapian::QueryParser::FLAG_WORD_BREAKS,
#elif defined(USE_XAPIAN_CJK_WORDS)
            Xapian::QueryParser::FLAG_CJK_WORDS,
#else
            Xapian::QueryParser::FLAG_CJK_NGRAM,
#endif
            prefix)};

#if defined(USE_XAPIAN_WORD_BREAKS) || defined(USE_XAPIAN_CJK_WORDS)
    // There is a bug in Xapian v1.5 and CJK word segmentation, in which
    // a term starting with a fullwidth Latin character such as U+FF21
    // is indexed with capitalization, but queried in small letter form
    // when parsed with parse_query.
    //
    // As a workaround, the following code checks if the query string
    // contains any characters in the Halfwidth and Fullwidth Forms
    // Unicode block. If so, it queries for either the query as
    // generated by the query parser, or the terms as generated during
    // indexing.
    //
    // XXX this should better be handled by normalizing characters in
    // that Unicode block during indexing.
    for (auto it = Xapian::Utf8Iterator(str); it != Xapian::Utf8Iterator(); ++it) {
        if (*it >= 0xff00 && *it <= 0xffef) {
            Xapian::TermGenerator *termgen = new_term_generator();
            Xapian::Document doc;
            termgen->set_document(doc);
            termgen->index_text(str, 1, prefix);
            Xapian::Query qq{Xapian::Query::OP_AND, doc.termlist_begin(), doc.termlist_end()};
            if (qq.get_length()) {
                *q |= qq;
            }
            delete termgen;
            break; // stop at first occurence of full/half-width character
        }
    }
#endif

    return q;
}

static Xapian::Query *xapian_query_new_match_internal(const xapian_db_t *db,
                                                      enum search_part partnum,
                                                      const char *str,
                                                      int convert_flags)
{
    charset_t utf8 = charset_lookupname("utf-8");
    char *mystr = charset_convert(str, utf8, convert_flags);
    charset_free(&utf8);
    if (!mystr) return NULL;

    static Xapian::Query *q = NULL;

    try {
        // Handle special value search parts.
        if (partnum == SEARCH_PART_LANGUAGE) {
            q = query_new_language(db, partnum, mystr);
        }
        else if (partnum == SEARCH_PART_PRIORITY) {
            q = query_new_priority(db, partnum, mystr);
        }
        else if (partnum == SEARCH_PART_LISTID) {
            q = query_new_listid(db, partnum, mystr);
        }
        else if (partnum == SEARCH_PART_FROM ||
                 partnum == SEARCH_PART_TO ||
                 partnum == SEARCH_PART_CC ||
                 partnum == SEARCH_PART_BCC ||
                 partnum == SEARCH_PART_DELIVEREDTO) {
            q = query_new_email(db, partnum, mystr);
        }
        else if (partnum == SEARCH_PART_TYPE) {
            q = query_new_type(db, partnum, mystr);
        }
        else if (partnum == SEARCH_PART_INREPLYTO ||
                partnum == SEARCH_PART_MESSAGEID ||
                partnum == SEARCH_PART_REFERENCES) {
            q = query_new_messageid(db, partnum, mystr);
        }
        else {
            // Match unstructured search parts
            int need_word_break = 0;
            for (const unsigned char *p = (const unsigned char *)mystr; *p; p++) {
                // Use ICU word break for Thaana codepage (0780) or higher.
                if (*p > 221) {
                    need_word_break = 1;
                    break;
                }
            }

            if (need_word_break) {
                q = xapian_query_new_match_word_break(db, partnum, mystr);
            }
            else {
                Xapian::TermGenerator::stem_strategy stem_strategy =
                    get_stem_strategy(partnum);
                q = query_new_textmatch(db, partnum, mystr, stem_strategy);
            }
            if (q && q->get_type() == Xapian::Query::LEAF_MATCH_NOTHING) {
                delete q;
                q = NULL;
            }
        }

    } catch (const Xapian::Error &err) {
        xsyslog(LOG_ERR, "IOERROR: caught exception",
                         "exception=<%s>",
                         err.get_description().c_str());
    }

    free(mystr);
    return q;
}

static bool query_terms_eq(const Xapian::Query *qa, const Xapian::Query *qb)
{
    if (qa == nullptr) {
        return qb == nullptr;
    }
    else if (qb == nullptr) {
        return qa == nullptr;
    }

    if (qa->get_type() != qb->get_type()) {
        return false;
    }

    Xapian::TermIterator ta = qa->get_unique_terms_begin();
    Xapian::TermIterator tb = qb->get_unique_terms_begin();
    Xapian::TermIterator ta_end = qa->get_unique_terms_end();
    Xapian::TermIterator tb_end = qb->get_unique_terms_end();

    for ( ; ta != ta_end && tb != tb_end; ++ta, ++tb) {
        if (*ta != *tb) return false;
    }

    if (ta != ta_end || tb != tb_end) {
        return false;
    }

    if (qa->get_num_subqueries() != qb->get_num_subqueries()) {
        return false;
    }

    for (size_t i = 0; i < qa->get_num_subqueries(); i++) {
        const Xapian::Query subqa = qa->get_subquery(i);
        const Xapian::Query subqb = qb->get_subquery(i);
        if (!query_terms_eq(&subqa, &subqb)) {
            return false;
        }
    }

    return true;
}

EXPORTED xapian_query_t *
xapian_query_new_match(const xapian_db_t *db, enum search_part partnum, const char *str)
{
    if (db->subdbs->empty()) {
        // no database to query
        return NULL;
    }

    unsigned min_version = *db->db_versions->begin();
    if (min_version < XAPIAN_DB_MIN_SUPPORTED_VERSION) {
        xsyslog(LOG_WARNING,
                "deprecated database version, reindex required",
                "version=<%d> min_supported_version=<%d> paths=<%s>",
                min_version, XAPIAN_DB_MIN_SUPPORTED_VERSION,
                db->paths->c_str());
    }

    unsigned max_version = *db->db_versions->rbegin();

    Xapian::Query *q = NULL;
    if (max_version >= 17) {
        // Versions 17 and above normalize terms using NFKC with CaseFolding.
        q = xapian_query_new_match_internal(db, partnum, str,
                CHARSET_UNORM_NFKC_CF | CHARSET_KEEPCASE | CHARSET_MIME_UTF8);
    }

    Xapian::Query *q_v16 = NULL;
    if (min_version <= 16) {
        /* Version 16 and earlier did not normalize unstemmed terms
         * and stemmed terms using Cyrus Search Form, a custom NFC. */
        q_v16 = xapian_query_new_match_internal(db, partnum, str,
                CHARSET_KEEPCASE | CHARSET_MIME_UTF8);
        if (query_terms_eq(q, q_v16)) {
            delete q_v16;
            q_v16 = NULL;
        }
    }

    Xapian::Query *q_v14 = NULL;
    if (min_version <= 14) {
        /* Version 14 and earlier indexed header fields in Cyrus search form */
        q_v14 = xapian_query_new_match_internal(db, partnum, str, charset_flags);
        if (query_terms_eq(q, q_v14)) {
            delete q_v14;
            q_v14 = NULL;
        }
    }

    // Combine queries to support legacy indexes.
    if (q && q_v14) {
        *q |= *q_v14;
    }
    else if (q_v14) {
        q = q_v14;
    }

    if (q && q_v16) {
        *q |= *q_v16;
    }
    else if (q_v16) {
        q = q_v16;
    }

    return (xapian_query_t*) q;
}

EXPORTED xapian_query_t *
xapian_query_new_compound(const xapian_db_t *db __attribute__((unused)),
                          int is_or, xapian_query_t **children, int n)
{
    try {
        // I want to use std::initializer_list<Xapian::Query*> here
        // but that requires "experimental" gcc C++0x support :(
        // 'compound' owns a refcount on each child.  We need to
        // drop the one we got when we allocated the children
        Xapian::Query* compound = new Xapian::Query;
        if (is_or)
            for (int i = 0 ; i < n ; i++) {
                *compound |= *(Xapian::Query*)children[i];
                delete (Xapian::Query*)children[i];
            }
        else
            for (int i = 0 ; i < n ; i++) {
                if (compound->empty())
                    *compound = *(Xapian::Query*)children[i];
                else
                    *compound &= *(Xapian::Query*)children[i];
                delete (Xapian::Query*)children[i];
            }
        return (xapian_query_t *)compound;
    }
    catch (const Xapian::Error &err) {
        xsyslog(LOG_ERR, "IOERROR: caught exception",
                         "exception=<%s>",
                         err.get_description().c_str());
        return 0;
    }
}

/* Xapian does not have an OP_NOT.  WTF?  We fake it with
 * OP_AND_NOT where the left child is MatchAll */
EXPORTED xapian_query_t *
xapian_query_new_not(const xapian_db_t *db __attribute__((unused)),
                     xapian_query_t *child)
{
    if (!child) return (xapian_query_t*) new Xapian::Query(Xapian::Query::MatchAll);

    try {
        Xapian::Query *qq = new Xapian::Query(
                                        Xapian::Query::OP_AND_NOT,
                                        Xapian::Query::MatchAll,
                                        *(Xapian::Query *)child);
        // 'compound' owns a refcount on each child.  We need to
        // drop the one we got when we allocated the children
        delete (Xapian::Query *)child;
        return (xapian_query_t *)qq;
    }
    catch (const Xapian::Error &err) {
        xsyslog(LOG_ERR, "IOERROR: caught exception",
                         "exception=<%s>",
                         err.get_description().c_str());
        return 0;
    }
}

EXPORTED xapian_query_t *
xapian_query_new_matchall(const xapian_db_t *db __attribute__((unused)))
{
    return (xapian_query_t *) new Xapian::Query(Xapian::Query::MatchAll);
}

EXPORTED xapian_query_t *
xapian_query_new_has_doctype(const xapian_db_t *db __attribute__((unused)),
                             char doctype, xapian_query_t *child)
{
    try {
        Xapian::Query *qq = new Xapian::Query(
                                        Xapian::Query::OP_FILTER,
                                        child ? *(Xapian::Query *)child : Xapian::Query::MatchAll,
                                        std::string("XE") + doctype);
        // 'compound' owns a refcount on each child.  We need to
        // drop the one we got when we allocated the children
        delete (Xapian::Query *)child;
        return (xapian_query_t *)qq;
    }
    catch (const Xapian::Error &err) {
        xsyslog(LOG_ERR, "IOERROR: caught exception",
                         "exception=<%s>",
                         err.get_description().c_str());
        return 0;
    }
}

EXPORTED void xapian_query_free(xapian_query_t *qq)
{
    try {
        delete (Xapian::Query *)qq;
    }
    catch (const Xapian::Error &err) {
        xsyslog(LOG_ERR, "IOERROR: caught exception",
                         "exception=<%s>",
                         err.get_description().c_str());
    }
}

EXPORTED int xapian_query_run(const xapian_db_t *db, const xapian_query_t *qq,
                              int (*cb)(void *data, size_t n, void *rock), void *rock)
{
    const Xapian::Query *query = (const Xapian::Query *)qq;
    void *data = NULL;
    size_t n = 0;

    if (!db->database) return 0;

    try {
        Xapian::Database *database = db->database;
        Xapian::Enquire enquire(*database);
        enquire.set_query(*query);
        enquire.set_sort_by_value(0, false); // sort by cyrusid ascending
        Xapian::MSet matches = enquire.get_mset(0, database->get_doccount());
        size_t size = matches.size();
        if (size) data = xzmalloc(size * 41);
        for (Xapian::MSetIterator i = matches.begin() ; i != matches.end() ; ++i) {
            const Xapian::Document& d = i.get_document();
            const std::string& cyrusid = d.get_value(SLOT_CYRUSID);

            /* ignore documents with no cyrusid.  Shouldn't happen, but has been seen */
            if (cyrusid.length() != 43) {
                xsyslog(LOG_ERR, "IOERROR: skipping document with zero-length cyrusid",
                                 "documentid=<%u> paths=<%s>",
                                 d.get_docid(), db->paths->c_str());
                continue;
            }
            const char *cstr = cyrusid.c_str();
            if (cstr[0] != '*' || !isalpha(cstr[1]) || cstr[2] != '*') {
                xsyslog(LOG_ERR, "IOERROR: skipping document with invalid cyrusid",
                                 "cyrusid=<%s> documentid=<%u> paths=<%s>",
                                 cstr, d.get_docid(), db->paths->c_str());
                continue;
            }
            if (n >= size) throw Xapian::DatabaseError("Too many records in MSet");
            char *entry = (char *) data + (41*n);
            memcpy(entry, cstr+3, 40);
            entry[40] = '\0';
            ++n;
        }
    }
    catch (const Xapian::Error &err) {
        xsyslog(LOG_ERR, "IOERROR: caught exception",
                         "exception=<%s> query=<%s>",
                         err.get_description().c_str(),
                         query ? query->get_description().c_str() : "");
        free(data);
        return IMAP_IOERROR;
    }

    if (!n) {
        free(data);
        return 0;
    }

    int r = cb(data, n, rock);
    free(data);
    return r;
}

EXPORTED void xapian_query_serialize(xapian_query_t *xq, struct buf *buf)
{
    if (!xq) return;
    const Xapian::Query *query = (const Xapian::Query *)xq;
    buf_appendcstr(buf, query->get_description().c_str());
}

/* ====================================================================== */

struct xapian_snipgen
{
    Xapian::Stem *default_stemmer;
    xapian_db_t *db;
    Xapian::Database *memdb;
    std::vector<std::string> *loose_terms;
    std::vector<std::string> *queries;
    char *cyrusid;
    char doctype;
    struct buf *buf;
    const char *hi_start;
    const char *hi_end;
    const char *omit;
    size_t max_len;
};

EXPORTED xapian_snipgen_t *
xapian_snipgen_new(xapian_db_t *db, const struct search_snippet_markup *markup)
{
    xapian_snipgen_t *snipgen = (xapian_snipgen_t *)xzmalloc(sizeof(xapian_snipgen_t));
    snipgen->default_stemmer = new Xapian::Stem(new CyrusSearchStemmer);
    snipgen->db = db;
    snipgen->memdb = new Xapian::WritableDatabase(std::string(), Xapian::DB_BACKEND_INMEMORY);
    snipgen->buf = buf_new();
    snipgen->hi_start = markup->hi_start;
    snipgen->hi_end = markup->hi_end;
    snipgen->omit = markup->omit;
    snipgen->max_len = (size_t) config_getint(IMAPOPT_SEARCH_SNIPPET_LENGTH);

    return snipgen;
}

EXPORTED void xapian_snipgen_free(xapian_snipgen_t *snipgen)
{
    if (!snipgen) return;
    delete snipgen->default_stemmer;
    delete snipgen->loose_terms;
    delete snipgen->queries;
    delete snipgen->memdb;
    free(snipgen->cyrusid);
    buf_destroy(snipgen->buf);
    free(snipgen);
}

static Xapian::Query xapian_snipgen_build_query(xapian_snipgen_t *snipgen, Xapian::Stem& stemmer)
{
    Xapian::TermGenerator term_generator;
    Xapian::Query q;

    if (snipgen->loose_terms) {
        /* Add loose query terms */
        term_generator.set_stemmer(stemmer);
#if defined(USE_XAPIAN_WORD_BREAKS)
        term_generator.set_flags(Xapian::TermGenerator::FLAG_WORD_BREAKS,
                ~Xapian::TermGenerator::FLAG_WORD_BREAKS);
#elif defined(USE_XAPIAN_CJK_WORDS)
        term_generator.set_flags(Xapian::TermGenerator::FLAG_CJK_WORDS,
                ~Xapian::TermGenerator::FLAG_CJK_WORDS);
#else
        term_generator.set_flags(Xapian::TermGenerator::FLAG_CJK_NGRAM,
                ~Xapian::TermGenerator::FLAG_CJK_NGRAM);
#endif

        for(size_t i = 0; i < snipgen->loose_terms->size(); ++i)
        {
            term_generator.index_text(Xapian::Utf8Iterator((*snipgen->loose_terms)[i]));
        }

        const Xapian::Document& doc = term_generator.get_document();
        q = Xapian::Query(Xapian::Query::OP_OR, doc.termlist_begin(), doc.termlist_end());
    }

    if (snipgen->queries) {
        /* Add phrase queries */
        unsigned flags = Xapian::QueryParser::FLAG_PHRASE|
                         Xapian::QueryParser::FLAG_WILDCARD|
#if defined(USE_XAPIAN_WORD_BREAKS)
                         Xapian::QueryParser::FLAG_WORD_BREAKS;
#elif defined(USE_XAPIAN_CJK_WORDS)
                         Xapian::QueryParser::FLAG_CJK_WORDS;
#else
                         Xapian::QueryParser::FLAG_CJK_NGRAM;
#endif
        Xapian::QueryParser queryparser;
        queryparser.set_stemmer(stemmer);
        for(size_t i = 0; i < snipgen->queries->size(); ++i) {
            q |= queryparser.parse_query((*snipgen->queries)[i], flags);;
        }
    }

    return q;
}

EXPORTED int xapian_snipgen_add_match(xapian_snipgen_t *snipgen,
                                      const char *match)
{
    size_t len = strlen(match);
    bool is_query = len > 1 && ((match[0] == '"' && match[len-1] == '"') ||
                                (strchr(match, '*') != NULL));

    if (is_query) {
        if (!snipgen->queries) {
            snipgen->queries = new std::vector<std::string>;
        }
        snipgen->queries->push_back(match);
    } else {
        if (!snipgen->loose_terms) {
            snipgen->loose_terms = new std::vector<std::string>;
        }
        snipgen->loose_terms->push_back(match);
    }

    return 0;
}

EXPORTED int xapian_snipgen_begin_doc(xapian_snipgen_t *snipgen,
                                      const struct message_guid *guid,
                                      char doctype)
{
    struct buf buf = BUF_INITIALIZER;
    make_cyrusid(&buf, guid, doctype);
    snipgen->cyrusid = buf_release(&buf);
    snipgen->doctype = doctype;

    buf_reset(snipgen->buf);
    return 0;
}

EXPORTED int xapian_snipgen_make_snippet(xapian_snipgen_t *snipgen,
                                         const struct buf *part,
                                         Xapian::Stem* stemmer)
{
    int r = 0;
    try {
        std::string text {buf_base(part), buf_len(part)};
        Xapian::Enquire enquire(*snipgen->memdb);
        Xapian::Query qq = xapian_snipgen_build_query(snipgen, *stemmer);
        if (qq.empty()) return 0;
        enquire.set_query(qq);

        unsigned flags = Xapian::MSet::SNIPPET_EXHAUSTIVE |
                         Xapian::MSet::SNIPPET_EMPTY_WITHOUT_MATCH;
#if defined(USE_XAPIAN_WORD_BREAKS)
        flags |= Xapian::MSet::SNIPPET_WORD_BREAKS;
#elif defined(USE_XAPIAN_CJK_WORDS)
        flags |= Xapian::MSet::SNIPPET_CJK_WORDS;
#endif

        const std::string snippet = enquire.get_mset(0, 0).snippet(text,
                snipgen->max_len - buf_len(snipgen->buf),
                *stemmer, flags,
                snipgen->hi_start,
                snipgen->hi_end,
                snipgen->omit);
        if (!snippet.empty()) {
            if (buf_len(snipgen->buf)) {
                buf_appendoverlap(snipgen->buf, snipgen->omit);
            }
            buf_appendcstr(snipgen->buf, snippet.c_str());
        }
    } catch (const Xapian::Error &err) {
        xsyslog(LOG_ERR, "IOERROR: caught exception",
                         "exception=<%s>",
                         err.get_description().c_str());
        r = IMAP_IOERROR;
    }
    return r;
}

EXPORTED int xapian_snipgen_doc_part(xapian_snipgen_t *snipgen,
                                     const struct buf *part)
{
    // Ignore empty queries.
    if (!snipgen->loose_terms && !snipgen->queries) return 0;

    // Don't exceed allowed snippet length.
    if (buf_len(snipgen->buf) >= snipgen->max_len) return 0;

    if (config_getswitch(IMAPOPT_SEARCH_INDEX_LANGUAGE) &&
        snipgen->db->database && snipgen->cyrusid) {
        std::set<std::string> doclangs;

        // Lookup stemmer language for this document part, if any.
        std::string key = lang_doc_key(snipgen->cyrusid);
        for (const Xapian::Database& subdb : *snipgen->db->subdbs) {
            std::string val = subdb.get_metadata(key);
            if (!val.empty() && isalpha(val[0])) parse_doclangs(val, doclangs);
            break;
        }

        // Generate snippets for each detected message language.
        // The first non-empty snippet wins.
        size_t prev_size = buf_len(snipgen->buf);
        for (const std::string& iso_lang : doclangs) {
            if (iso_lang != "en") {
                try {
                    Xapian::Stem stemmer = get_stemmer(iso_lang);
                    int r = xapian_snipgen_make_snippet(snipgen, part, &stemmer);
                    if (!r && prev_size != buf_len(snipgen->buf)) {
                        return 0;
                    }
                } catch (const Xapian::InvalidArgumentError &err) {
                    // ignore unknown stemmer
                }
            }
        }
    }

    /* Using a custom stemmer did not generate a snippet.
     * This could be because the query matched using the
     * default stemmer, so try generating a snippet with
     * that stemmer instead.*/
    return xapian_snipgen_make_snippet(snipgen, part, snipgen->default_stemmer);
}

EXPORTED int xapian_snipgen_end_doc(xapian_snipgen_t *snipgen, struct buf *buf)
{
    buf_reset(buf);
    buf_copy(buf, snipgen->buf);
    buf_cstring(buf);
    buf_reset(snipgen->buf);

    delete snipgen->loose_terms;
    snipgen->loose_terms = NULL;

    delete snipgen->queries;
    snipgen->queries = NULL;

    free(snipgen->cyrusid);
    snipgen->cyrusid = NULL;
    snipgen->doctype = 0;

    return 0;
}

/* cb returns true if document should be copied, false if not */
EXPORTED int xapian_filter(const char *dest, const char **sources,
                           int (*cb)(const char *cyrusid, void *rock),
                           void *rock)
{
    int r = 0;
    const char *thispath = "(unknown path)";

    try {
        /* create a destination database */
        Xapian::WritableDatabase destdb {dest, Xapian::DB_CREATE|Xapian::DB_BACKEND_GLASS};

        /* With multiple databases as above, the docids are interleaved, so it
         * might be worth trying to open each source and copy its documents to
         * destdb in turn for better locality of reference, and so better cache
         * use. -- Olly on the mailing list */

        std::vector<Xapian::Database> srcdbs;

        // Open databases and aggregate database-level metadata.
        while (*sources) {
            thispath = *sources++;
            const Xapian::Database srcdb {thispath};
            srcdbs.push_back(srcdb);
        }

        // Copy all matching documents.
        std::set<int> db_versions;

        for (size_t i = 0; i < srcdbs.size(); ++i) {
            const Xapian::Database& srcdb = srcdbs.at(i);
            bool need_md_versions = false;
            std::set<int> md_versions = read_db_versions(srcdb);

            /* copy all matching documents to the new DB */
            for (Xapian::ValueIterator it = srcdb.valuestream_begin(SLOT_CYRUSID);
                    it != srcdb.valuestream_end(SLOT_CYRUSID); ++it) {
                const std::string& cyrusid = *it;
                const std::string idkey {"cyrusid." + cyrusid};

                // check if caller wants this cyrusid
                if (!cb(cyrusid.c_str(), rock)) {
                    continue;
                }

                // is it already indexed?
                if (!destdb.get_metadata(idkey).empty()) {
                    continue;
                }

                // is there a subsequent db with a better index level? (only for G docs)
                uint8_t indexlevel = parse_indexlevel(srcdb.get_metadata(idkey));
                if (cyrusid[1] == XAPIAN_WRAP_DOCTYPE_MSG) {
                    int found_better = 0;
                    for (size_t j = i + 1; !found_better && j < srcdbs.size(); ++j) {
                        uint8_t level = parse_indexlevel(srcdbs[j].get_metadata(idkey));
                        found_better = better_indexlevel(indexlevel, level) != indexlevel;
                    }
                    if (found_better) {
                        continue;
                    }
                }

                // add document
                Xapian::Document srcdoc = srcdb.get_document(it.get_docid());
                Xapian::docid docid = destdb.add_document(srcdoc);
                destdb.set_metadata(idkey, format_indexlevel(indexlevel));

                // copy document language metadata
                const std::string& langkey = lang_doc_key(cyrusid.c_str());
                if (destdb.get_metadata(langkey).empty()) {
                    std::string val = srcdb.get_metadata(langkey);
                    if (!val.empty() && isalpha(val[0])) {
                        destdb.set_metadata(langkey, val);
                    }
                }
                const std::string& langval = srcdoc.get_value(SLOT_DOCLANGS);
                if (!langval.empty() && !isalpha(langval[0])) {
                    destdb.get_document(docid).remove_value(SLOT_DOCLANGS);
                }
                // add document index version
                const std::string& verval = srcdoc.get_value(SLOT_INDEXVERSION);
                if (!verval.empty()) {
                    int version = std::atoi(verval.c_str());
                    if (version) db_versions.insert(version);
                }
                else need_md_versions = true;
            }

            if (need_md_versions) {
                /* At least one added document didn't have its index
                 * version slot set in this subdb. Read legacy versions. */
                std::set<int> md_versions = read_db_versions(srcdb);
                db_versions.insert(md_versions.begin(), md_versions.lower_bound(14));
            }
        }

        thispath = "(unknown path)";

        // set the versions
        write_db_versions(destdb, db_versions);

        // recalculate language counts
        std::map<const std::string, unsigned> lang_counts;
        r = calculate_language_counts(destdb, lang_counts);
        if (r) {
            xsyslog(LOG_ERR, "IOERROR: corrupt metadata",
                             "filter=<%s>",
                             dest);
            return r;
        }
        write_language_counts(destdb, lang_counts);

        /* commit all changes explicitly */
        destdb.commit();
    }
    catch (const Xapian::Error &err) {
        xsyslog(LOG_ERR, "IOERROR: caught exception",
                         "exception=<%s> path=<%s>",
                         err.get_description().c_str(), thispath);
        r = IMAP_IOERROR;
    }

    return r;
}

EXPORTED const char *xapian_version_string()
{
    return Xapian::version_string();
}

struct xapian_doc {
    Xapian::TermGenerator termgen;
    Xapian::Document doc;
};

EXPORTED xapian_doc_t *xapian_doc_new(void)
{
    xapian_doc_t *doc = new xapian_doc;
    doc->termgen.set_document(doc->doc);
    return doc;
}

EXPORTED void xapian_doc_index_text(xapian_doc_t *doc,
                                    const char *text, size_t len)
{
    doc->termgen.index_text(Xapian::Utf8Iterator(text, len));
}

EXPORTED size_t xapian_doc_termcount(xapian_doc_t *doc)
{
    return doc->doc.termlist_count();
}

EXPORTED int xapian_doc_foreach_term(xapian_doc_t *doc,
                                     int(*cb)(const char*, void*),
                                     void *rock)
{
    for (Xapian::TermIterator ti = doc->doc.termlist_begin();
            ti != doc->doc.termlist_end(); ++ti) {
        int r = cb((*ti).c_str(), rock);
        if (r) return r;
    }
    return 0;
}

EXPORTED void xapian_doc_reset(xapian_doc_t *doc)
{
    doc->doc.clear_values();
}

EXPORTED void xapian_doc_close(xapian_doc_t *doc)
{
    delete doc;
}

EXPORTED int xapian_charset_flags(int flags)
{
    return (flags |
            CHARSET_KEEPCASE |
            CHARSET_MIME_UTF8 |
            CHARSET_UNORM_NFKC_CF) &
        ~CHARSET_SKIPDIACRIT;
}
