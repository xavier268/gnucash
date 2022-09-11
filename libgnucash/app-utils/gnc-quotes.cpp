/********************************************************************\
 * gnc-quotes.hpp -- proxy for Finance::Quote                       *
 * Copyright (C) 2021 Geert Janssens <geert@kobaltwit.be>           *
 *                                                                  *
 * This program is free software; you can redistribute it and/or    *
 * modify it under the terms of the GNU General Public License as   *
 * published by the Free Software Foundation; either version 2 of   *
 * the License, or (at your option) any later version.              *
 *                                                                  *
 * This program is distributed in the hope that it will be useful,  *
 * but WITHOUT ANY WARRANTY; without even the implied warranty of   *
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the    *
 * GNU General Public License for more details.                     *
 *                                                                  *
 * You should have received a copy of the GNU General Public License*
 * along with this program; if not, contact:                        *
 *                                                                  *
 * Free Software Foundation           Voice:  +1-617-542-5942       *
 * 51 Franklin Street, Fifth Floor    Fax:    +1-617-542-2652       *
 * Boston, MA  02110-1301,  USA       gnu@gnu.org                   *
\ *******************************************************************/

#include <config.h>

#include <algorithm>
#include <stdexcept>
#include <vector>
#include <string>
#include <iostream>
#include <boost/algorithm/string.hpp>
#include <boost/filesystem.hpp>
#include <boost/process.hpp>
#include <boost/regex.hpp>
#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <boost/iostreams/device/array.hpp>
#include <boost/iostreams/stream_buffer.hpp>
#include <boost/locale.hpp>
#include <boost/asio.hpp>
#include <glib.h>
#include "gnc-commodity.hpp"
#include <gnc-datetime.hpp>
#include <gnc-numeric.hpp>
#include "gnc-quotes.hpp"

extern "C" {
#include <gnc-commodity.h>
#include <gnc-path.h>
#include "gnc-ui-util.h"
#include <gnc-prefs.h>
#include <gnc-session.h>
#include <regex.h>
#include <qofbook.h>
}

static const QofLogModule log_module = "gnc.price-quotes";

namespace bl = boost::locale;
namespace bp = boost::process;
namespace bfs = boost::filesystem;
namespace bpt = boost::property_tree;
namespace bio = boost::iostreams;

using QuoteResult = std::tuple<int, StrVec, StrVec>;

struct GncQuoteSourceError : public std::runtime_error
{
    GncQuoteSourceError(const std::string& err) : std::runtime_error(err) {}
};

CommVec
gnc_quotes_get_quotable_commodities(const gnc_commodity_table * table);

class GncQuoteSource
{
public:
    virtual ~GncQuoteSource() = default;
    virtual const StrVec& get_sources() const noexcept = 0;
    virtual const std::string & get_version() const noexcept = 0;
    virtual QuoteResult get_quotes(const std::string& json_str) const = 0;
    virtual bool usable() const noexcept = 0;
};

class GncQuotesImpl
{
public:
    // Constructor - checks for presence of Finance::Quote and import version and quote sources
    GncQuotesImpl ();
    explicit GncQuotesImpl (QofBook *book);
    GncQuotesImpl(QofBook*, std::unique_ptr<GncQuoteSource>);

    void fetch (QofBook *book);
    void fetch (CommVec& commodities);
    void fetch (gnc_commodity *comm);

    const std::string& version() noexcept { return m_version.empty() ? not_found : m_version; }
    const QuoteSources& sources() noexcept { return m_sources; }
    GList* sources_as_glist ();

private:
    std::string query_fq (const CommVec&);
    void parse_quotes (const std::string& quote_str, const CommVec& commodities);
    std::string comm_vec_to_json_string(const CommVec&) const;
    GNCPrice* parse_one_quote(const bpt::ptree&, gnc_commodity*);

    std::unique_ptr<GncQuoteSource> m_quotesource;
    std::string m_version;
    QuoteSources m_sources;
    QofBook *m_book;
    gnc_commodity *m_dflt_curr;
};

class GncFQQuoteSource final : public GncQuoteSource
{
    const bfs::path c_cmd;
    const std::string c_fq_wrapper;
    bool m_ready;
    std::string m_version;
    StrVec m_sources;
public:
    GncFQQuoteSource();
    ~GncFQQuoteSource() = default;
    virtual const std::string& get_version() const noexcept override { return m_version; }
    virtual const StrVec& get_sources() const noexcept override { return m_sources; }
    virtual QuoteResult get_quotes(const std::string&) const override;
    virtual bool usable() const noexcept override { return m_ready; }
private:
    QuoteResult run_cmd (const StrVec& args, const std::string& json_string) const;

};

GncFQQuoteSource::GncFQQuoteSource() :
c_cmd{bp::search_path("perl")},
c_fq_wrapper{std::string(gnc_path_get_bindir()) + "/finance-quote-wrapper"},
m_ready{false},
m_version{}, m_sources{}
{
    StrVec args{"-w", c_fq_wrapper, "-v"};
    const std::string empty_string;
    auto [rv, sources, errors] = run_cmd(args, empty_string);
    if (rv)
    {
        std::string err{bl::translate("Failed to initialize Finance::Quote: ")};
        for (auto err_line : errors)
            err += err_line.empty() ? "" : err_line + "\n";
        throw(GncQuoteSourceError(err));
    }
    if (!errors.empty())
    {
        std::string err{bl::translate("Finance::Quote check returned error ")};
        for(const auto& err_line : errors)
            err += err.empty() ? "" : err_line + "\n";
        throw(GncQuoteSourceError(err));
    }
    static const boost::regex version_fmt{"[0-9]\\.[0-9][0-9]"};
    auto version{sources.front()};
    if (version.empty() || !boost::regex_match(version, version_fmt))
    {
        std::string err{bl::translate("Invalid Finance::Quote Version ")};
            err +=  version.empty() ? "" : version;
        throw(GncQuoteSourceError(err));
    }
    m_ready = true;
    sources.erase(sources.begin());
    m_sources = std::move(sources);
}

QuoteResult
GncFQQuoteSource::get_quotes(const std::string& json_str) const
{
    StrVec args{"-w", c_fq_wrapper, "-f" };
    return run_cmd(args, json_str);
}

QuoteResult
GncFQQuoteSource::run_cmd (const StrVec& args, const std::string& json_string) const
{
    StrVec out_vec, err_vec;
    int cmd_result;

    auto av_key = gnc_prefs_get_string ("general.finance-quote", "alphavantage-api-key");
    if (!av_key)
        PWARN("No Alpha Vantage API key set, currency quotes and other AlphaVantage based quotes won't work.");

    try
    {
        std::future<std::vector<char> > out_buf, err_buf;
        boost::asio::io_service svc;

        auto input_buf = bp::buffer (json_string);
        bp::child process (c_cmd, args,
                           bp::std_out > out_buf,
                           bp::std_err > err_buf,
                           bp::std_in < input_buf,
                           bp::env["ALPHAVANTAGE_API_KEY"]= (av_key ? av_key : ""),
                           svc);
        svc.run();
        process.wait();

        {
            auto raw = out_buf.get();
            std::vector<std::string> data;
            std::string line;
            bio::stream_buffer<bio::array_source> sb(raw.data(), raw.size());
            std::istream is(&sb);

            while (std::getline(is, line) && !line.empty())
                out_vec.push_back (std::move(line));

            raw = err_buf.get();
            bio::stream_buffer<bio::array_source> eb(raw.data(), raw.size());
            std::istream es(&eb);

            while (std::getline(es, line) && !line.empty())
                err_vec.push_back (std::move(line));
        }
        cmd_result = process.exit_code();
    }
    catch (std::exception &e)
    {
        cmd_result = -1;
        err_vec.push_back(e.what());
    };

    return QuoteResult (cmd_result, std::move(out_vec), std::move(err_vec));
}

/* GncQuotes implementation */
GncQuotesImpl::GncQuotesImpl() : m_quotesource{new GncFQQuoteSource},
m_version{}, m_sources{}, m_book{qof_session_get_book(gnc_get_current_session())},
m_dflt_curr{gnc_default_currency()}
{
    if (!m_quotesource->usable())
        return;
    m_sources = m_quotesource->get_sources();
}

GncQuotesImpl::GncQuotesImpl(QofBook* book) : m_quotesource{new GncFQQuoteSource},
m_version{}, m_sources{}, m_book{book},
m_dflt_curr{gnc_default_currency()}
{
    if (!m_quotesource->usable())
        return;
    m_sources = m_quotesource->get_sources();
}

GncQuotesImpl::GncQuotesImpl(QofBook* book, std::unique_ptr<GncQuoteSource> quote_source) :
m_quotesource{std::move(quote_source)},
m_version{}, m_sources{}, m_book{book}, m_dflt_curr{gnc_default_currency()}
{
    if (!m_quotesource->usable())
        return;
    m_sources = m_quotesource->get_sources();
}

GList*
GncQuotesImpl::sources_as_glist()
{
    GList* slist = nullptr;
    std::for_each (m_sources.rbegin(), m_sources.rend(),
                    [&slist](const std::string& source) { slist  = g_list_prepend (slist, g_strdup(source.c_str())); });
    return slist;
}


void
GncQuotesImpl::fetch (QofBook *book)
{
    if (!book)
        throw (GncQuoteException(bl::translate("GncQuotes::Fetch called with no book.")));
    auto commodities = gnc_quotes_get_quotable_commodities (
        gnc_commodity_table_get_table (book));
    fetch (commodities);
}

void
GncQuotesImpl::fetch (gnc_commodity *comm)
{
    auto commodities = CommVec {comm};
    fetch (commodities);
}

void
GncQuotesImpl::fetch (CommVec& commodities)
{
    if (commodities.empty())
        return;

    auto quote_str{query_fq (commodities)};
    parse_quotes (quote_str, commodities);
}

std::string
GncQuotesImpl::comm_vec_to_json_string (const CommVec& comm_vec) const
{
    bpt::ptree pt, pt_child;
    pt.put ("defaultcurrency", gnc_commodity_get_mnemonic (m_dflt_curr));

    std::for_each (comm_vec.cbegin(), comm_vec.cend(),
                   [this, &pt] (auto comm)
                   {
                       auto comm_mnemonic = gnc_commodity_get_mnemonic (comm);
                       auto comm_ns = std::string("currency");
                       if (gnc_commodity_is_currency (comm))
                       {
                           if (gnc_commodity_equiv(comm, m_dflt_curr) ||
                               (!comm_mnemonic || (strcmp (comm_mnemonic, "XXX") == 0)))
                               return;
                       }
                       else
                           comm_ns = gnc_quote_source_get_internal_name (gnc_commodity_get_quote_source (comm));

                       auto key = comm_ns + "." + comm_mnemonic;
                       pt.put (key, "");
                   }
    );

    std::ostringstream result;
    bpt::write_json(result, pt);
    return result.str();
}

std::string
GncQuotesImpl::query_fq (const CommVec& comm_vec)
{
    auto json_str{comm_vec_to_json_string(comm_vec)};
    auto [rv, quotes, errors] = m_quotesource->get_quotes(json_str);
    std::string answer;

    if (rv == 0)
    {
        for (auto line : quotes)
            answer.append(line + "\n");
    }
    else
    {
        std::string err_str;
        for (auto line: errors)
            err_str.append(line + "\n");
        throw(GncQuoteException(err_str));
    }

    return answer;
//        for (auto line : quotes)
//            PINFO("Output line retrieved from wrapper:\n%s", line.c_str());
//
//     for (auto line : errors)
//         PINFO("Error line retrieved from wrapper:\n%s",line.c_str());˚

}

struct PriceParams
{
    const char* ns;
    const char* mnemonic;
    bool success;
    std::string type;
    boost::optional<std::string> price;
    bool inverted;
    boost::optional<std::string> date;
    boost::optional<std::string> time;
    boost::optional<std::string> currency;
    boost::optional<std::string> errormsg;
};

static void
get_price_and_type(PriceParams& p, const bpt::ptree& comm_pt)
{
    p.type = "last";
    p.price = comm_pt.get_optional<std::string> (p.type);
    if (!p.price)
    {
        p.type = "nav";
        p.price = comm_pt.get_optional<std::string> (p.type);
    }
    if (!p.price)
    {
        p.type = "price";
        p.price = comm_pt.get_optional<std::string> (p.type);
        /* guile wrapper used "unknown" as price type when "price" was found,
         * reproducing here to keep same result for users in the pricedb */
        p.type = p.price ? "unknown" : "missing";
    }
}

static void
parse_quote_json(PriceParams& p, const bpt::ptree& comm_pt)
{
    auto success = comm_pt.get_optional<bool> ("success");
    p.success = success && *success;
    if (!p.success)
        p.errormsg = comm_pt.get_optional<std::string> ("errormsg");
    get_price_and_type(p, comm_pt);
    auto inverted = comm_pt.get_optional<bool> ("inverted");
    p.inverted = inverted && *inverted;
    p.date = comm_pt.get_optional<std::string> ("date");
    p.time = comm_pt.get_optional<std::string> ("time");
    p.currency = comm_pt.get_optional<std::string> ("currency");


    PINFO("Commodity: %s", p.mnemonic);
    PINFO("  Success: %s", (inverted ? "yes" : "no"));
    PINFO("     Date: %s", (p.date ? p.date->c_str() : "missing"));
    PINFO("     Time: %s", (p.time ? p.time->c_str() : "missing"));
    PINFO(" Currency: %s", (p.currency ? p.currency->c_str() : "missing"));
    PINFO("    Price: %s", (p.price ? p.price->c_str() : "missing"));
    PINFO(" Inverted: %s\n", (inverted ? "yes" : "no"));
}

static time64
calc_price_time(const PriceParams& p)
{
    time64 quote_time;
    std::string iso_date_str = GncDate().format ("%Y-%m-%d");
    if (p.date)
    {
        // Returned date is always in MM/DD/YYYY format according to F::Q man page, transform it to simplify conversion to GncDateTime
        auto date_tmp = *p.date;
        iso_date_str = date_tmp.substr (6, 4) + "-" + date_tmp.substr (0, 2) + "-" + date_tmp.substr (3, 2);
    }
    else
        PINFO("Info: no date  was returned for %s:%s - will use today %s",
              p.ns, p.mnemonic,
              (iso_date_str += " " + (p.time ? *p.time : "12:00:00")).c_str());

    auto can_convert = true;
    try
    {
        quote_time = static_cast<time64>(GncDateTime{iso_date_str});
    }
    catch (...)
    {
        PINFO("Warning: failed to parse quote date and time '%s' for %s:%s - will use today",
              iso_date_str.c_str(),  p.ns, p.mnemonic);
        quote_time = static_cast<time64>(GncDateTime());
    }

    return quote_time;
}

static boost::optional<GncNumeric>
get_price(const PriceParams& p)
{
    boost::optional<GncNumeric> price;
    try
    {
        price = GncNumeric { *p.price };
    }
    catch (...)
    {
        PWARN("Skipped %s:%s - failed to parse returned price '%s'",
              p.ns, p.mnemonic, p.price->c_str());
    }

    if (price && p.inverted)
        *price = price->inv();

    return price;
}

static gnc_commodity*
get_currency(const PriceParams& p, QofBook* book)
{
    if (!p.currency)
    {
        PWARN("Skipped %s:%s - Finance::Quote didn't return a currency",
              p.ns, p.mnemonic);
        return nullptr;
    }
    std::string curr_str = *p.currency;
    boost::to_upper (curr_str);
    auto commodity_table = gnc_commodity_table_get_table (book);
    auto currency = gnc_commodity_table_lookup (commodity_table, "ISO4217", curr_str.c_str());

    if (!currency)
    {
        PWARN("Skipped %s:%s  - failed to parse returned currency '%s'",
              p.ns, p.mnemonic, p.currency->c_str());
        return nullptr;
    }

    return currency;
}

GNCPrice*
GncQuotesImpl::parse_one_quote(const bpt::ptree& pt, gnc_commodity* comm)
{
    PriceParams p;
    p.ns = gnc_commodity_get_namespace (comm);
    p.mnemonic = gnc_commodity_get_mnemonic (comm);
    if (gnc_commodity_equiv(comm, m_dflt_curr) ||
        (!p.mnemonic || (strcmp (p.mnemonic, "XXX") == 0)))
        return nullptr;
    auto comm_pt_ai{pt.find(p.mnemonic)};
    if (comm_pt_ai == pt.not_found())
    {
        PINFO("Skipped %s:%s - Finance::Quote didn't return any data.",
              p.ns, p.mnemonic);
        return nullptr;
    }

    auto comm_pt{comm_pt_ai->second};
    parse_quote_json(p, comm_pt);

    if (!p.success)
    {
        PWARN("Skipped %s:%s - Finance::Quote returned fetch failure.\nReason %s",
              p.ns, p.mnemonic,
              (p.errormsg ? p.errormsg->c_str() : "unknown"));
        return nullptr;
    }

    if (!p.price)
    {
        PWARN("Skipped %s:%s - Finance::Quote didn't return a valid price",
              p.ns, p.mnemonic);
        return nullptr;
    }

    auto price{get_price(p)};
    if (!price)
        return nullptr;

    auto currency{get_currency(p, m_book)};
    if (!currency)
        return nullptr;

    auto quotedt{calc_price_time(p)};
    auto gnc_price = gnc_price_create (m_book);
    gnc_price_begin_edit (gnc_price);
    gnc_price_set_commodity (gnc_price, comm);
    gnc_price_set_currency (gnc_price, currency);
    gnc_price_set_time64 (gnc_price, static_cast<time64> (quotedt));
    gnc_price_set_source (gnc_price, PRICE_SOURCE_FQ);
    gnc_price_set_typestr (gnc_price, p.type.c_str());
    gnc_price_set_value (gnc_price, *price);
    gnc_price_commit_edit (gnc_price);
    return gnc_price;
}

void
GncQuotesImpl::parse_quotes (const std::string& quote_str, const CommVec& comm_vec)
{
    bpt::ptree pt;
    std::istringstream ss {quote_str};
    const char* what = nullptr;

    try
    {
        bpt::read_json (ss, pt);
    }
    catch (bpt::json_parser_error &e) {
        what = e.what();
    }
    catch (const std::runtime_error& e)
    {
        what = e.what();
    }
    catch (const std::logic_error& e)
    {
        what = e.what();
    }
    catch (...) {
        std::string error_msg{_("Failed to parse result returned by Finance::Quote.")};
        throw(GncQuoteException(error_msg));
    }
    if (what)
    {
        std::string error_msg{_("Failed to parse result returned by Finance::Quote.")};
        error_msg += "\n";
        error_msg += _("Error message:");
        error_msg += "\n";
        error_msg += what;
        throw(GncQuoteException(error_msg));
    }

    auto pricedb{gnc_pricedb_get_db(m_book)};
    for (auto comm : comm_vec)
    {
        auto price{parse_one_quote(pt, comm)};
        if (!price)
            continue;
        gnc_price_begin_edit (price);
        gnc_pricedb_add_price(pricedb, price);
        gnc_price_commit_edit(price);
        gnc_price_unref (price);
    }
}



/********************************************************************
 * gnc_quotes_get_quotable_commodities
 * list commodities in a given namespace that get price quotes
 ********************************************************************/
/* Helper function to be passed to g_list_for_each applied to the result
 * of gnc_commodity_namespace_get_commodity_list.
 */
static void
get_quotables_helper1 (gpointer value, gpointer data)
{
    auto l = static_cast<CommVec *> (data);
    auto comm = static_cast<gnc_commodity *> (value);
    auto quote_flag = gnc_commodity_get_quote_flag (comm);
    auto quote_source = gnc_commodity_get_quote_source (comm);
    auto quote_source_supported = gnc_quote_source_get_supported (quote_source);

    if (!quote_flag ||
        !quote_source || !quote_source_supported)
        return;
    l->push_back (comm);
}

// Helper function to be passed to gnc_commodity_table_for_each
static gboolean
get_quotables_helper2 (gnc_commodity *comm, gpointer data)
{
    auto l = static_cast<CommVec *> (data);
    auto quote_flag = gnc_commodity_get_quote_flag (comm);
    auto quote_source = gnc_commodity_get_quote_source (comm);
    auto quote_source_supported = gnc_quote_source_get_supported (quote_source);

    if (!quote_flag ||
        !quote_source || !quote_source_supported)
        return TRUE;
    l->push_back (comm);
    return TRUE;
}

CommVec
gnc_quotes_get_quotable_commodities (const gnc_commodity_table * table)
{
    gnc_commodity_namespace * ns = NULL;
    const char *name_space;
    GList * nslist, * tmp;
    CommVec l;
    regex_t pattern;
    const char *expression = gnc_prefs_get_namespace_regexp ();

    // ENTER("table=%p, expression=%s", table, expression);
    if (!table)
        return CommVec ();

    if (expression && *expression)
    {
        if (regcomp (&pattern, expression, REG_EXTENDED | REG_ICASE) != 0)
        {
            // LEAVE ("Cannot compile regex");
            return CommVec ();
        }

        nslist = gnc_commodity_table_get_namespaces (table);
        for (tmp = nslist; tmp; tmp = tmp->next)
        {
            name_space = static_cast<const char *> (tmp->data);
            if (regexec (&pattern, name_space, 0, NULL, 0) == 0)
            {
                // DEBUG ("Running list of %s commodities", name_space);
                ns = gnc_commodity_table_find_namespace (table, name_space);
                if (ns)
                {
                    auto cm_list = gnc_commodity_namespace_get_commodity_list (ns);
                    g_list_foreach (cm_list, &get_quotables_helper1, (gpointer) &l);
                }
            }
        }
        g_list_free (nslist);
        regfree (&pattern);
    }
    else
    {
        gnc_commodity_table_foreach_commodity (table, get_quotables_helper2,
                                               (gpointer) &l);
    }
    //LEAVE ("list head %p", &l);
    return l;
}

/* Public interface functions */
// Constructor - checks for presence of Finance::Quote and import version and quote sources
GncQuotes::GncQuotes ()
{
    try
    {
        m_impl = std::make_unique<GncQuotesImpl>();
    }
    catch (const GncQuoteSourceError& err)
    {
        throw(GncQuoteException(err.what()));
    }
}


void
GncQuotes::fetch (QofBook *book)
{
    m_impl->fetch (book);
}

void GncQuotes::fetch (CommVec& commodities)
{
    m_impl->fetch (commodities);
}

void GncQuotes::fetch (gnc_commodity *comm)
{
    m_impl->fetch (comm);
}

const std::string& GncQuotes::version() noexcept
{
    return m_impl->version ();
}

const QuoteSources& GncQuotes::sources() noexcept
{
    return m_impl->sources ();
}

GList* GncQuotes::sources_as_glist ()
{
    return m_impl->sources_as_glist ();
}

GncQuotes::~GncQuotes() = default;

