#if defined(__GNUG__) && __GNUG__ < 3
#define _XOPEN_SOURCE
#endif

#include "journal.h"
#include "textual.h"
#include "datetime.h"
#include "valexpr.h"
#include "error.h"
#include "option.h"
#include "config.h"
#include "timing.h"
#include "util.h"
#include "acconf.h"

#include <fstream>
#include <sstream>
#include <cstring>
#include <ctime>
#include <cctype>
#include <cstdio>
#include <cstdlib>

#if defined(__GNUG__) && __GNUG__ < 3
extern "C" char *realpath(const char *, char resolved_path[]);
#endif

#define TIMELOG_SUPPORT 1

namespace ledger {

#define MAX_LINE 1024

static std::string  path;
static unsigned int linenum;
static unsigned int src_idx;
static accounts_map account_aliases;

#ifdef TIMELOG_SUPPORT
static std::time_t time_in;
static account_t * last_account;
static std::string last_desc;
#endif

inline char * next_element(char * buf, bool variable = false)
{
  for (char * p = buf; *p; p++) {
    if (! (*p == ' ' || *p == '\t'))
      continue;

    if (! variable) {
      *p = '\0';
      return skip_ws(p + 1);
    }
    else if (*p == '\t') {
      *p = '\0';
      return skip_ws(p + 1);
    }
    else if (*(p + 1) == ' ') {
      *p = '\0';
      return skip_ws(p + 2);
    }
  }
  return NULL;
}

static inline bool is_mathchr(const char c) {
  return (c == '(' || c == ')' ||
	  c == '+' || c == '-' ||
	  c == '*' || c == '/');
}

static inline void copy_wsbuf(char *& q, char *& wq, char * wsbuf) {
  *wq = '\0';
  std::strcpy(q, wsbuf);
  q += std::strlen(wsbuf);
  wq = wsbuf;
}

static char * parse_inline_math(const char * expr)
{
  char * buf	 = new char[std::strlen(expr) * 2];
  char * q	 = buf;
  char   wsbuf[64];
  char * wq      = wsbuf;
  bool	 in_math = true;
  bool	 could   = true;

  *q++ = '(';

  for (const char * p = expr; *p; p++) {
    if (std::isspace(*p)) {
      *wq++ = *p;
    } else {
      bool saw_math = is_mathchr(*p);
      if (in_math && ! saw_math) {
	copy_wsbuf(q, wq, wsbuf);
	*q++ = '{';
	in_math = could = false;
      }
      else if (! in_math && saw_math && could) {
	*q++ = '}';
	copy_wsbuf(q, wq, wsbuf);
	in_math = true;
      }
      else if (wq != wsbuf) {
	copy_wsbuf(q, wq, wsbuf);
      }

      if (! in_math && std::isdigit(*p))
	could = true;

      *q++ = *p;
    }
  }

  if (! in_math)
    *q++ = '}';

  *q++ = ')';
  *q++ = '\0';

  DEBUG_PRINT("ledger.textual.inlinemath",
	      "Parsed '" << expr << "' as '" << buf << "'");

  return buf;
}

value_expr_t * parse_amount(const char * text, amount_t& amt,
			    unsigned short flags, transaction_t& xact)
{
  char * altbuf = NULL;

  if (*text && *text != '(' && *text != '-') {
    bool in_quote   = false;
    bool seen_digit = false;
    for (const char * p = text + 1; *p; p++)
      if (*p == '"') {
	in_quote = ! in_quote;
      }
      else if (! in_quote) {
	if (is_mathchr(*p)) {
	  if (*p == '-' && ! seen_digit)
	    continue;
	  text = altbuf = parse_inline_math(text);
	  break;
	}
	else if (std::isdigit(*p)) {
	  seen_digit = true;
	}
      }
  }

  value_expr_t * expr = NULL;

  if (*text != '(') {
    amt.parse(text, flags);

    if (altbuf)
      delete[] altbuf;
  } else {
    expr = parse_value_expr(text);

    if (altbuf)
      delete[] altbuf;

    if (! compute_amount(expr, amt, xact))
      throw parse_error(path, linenum, "Value expression yields a balance");
  }

  return expr;
}

transaction_t * parse_transaction(char * line, account_t * account)
{
  // The account will be determined later...
  std::auto_ptr<transaction_t> xact(new transaction_t(NULL));

  // The call to `next_element' will skip past the account name, and
  // return a pointer to the beginning of the amount.  Once we know
  // where the amount is, we can strip off any transaction note, and
  // parse it.

  char * amount	  = NULL;
  char * price	  = NULL;
  bool	 per_unit = true;

  char * p = skip_ws(line);
  switch (*p) {
  case '*':
    xact->state = transaction_t::CLEARED;
    p = skip_ws(++p);
    break;
  case '!':
    xact->state = transaction_t::PENDING;
    p = skip_ws(++p);
    break;
  }

  if (amount = next_element(p, true)) {
    amount = skip_ws(amount);
    if (! *amount)
      amount = NULL;
    else {
      if (char * note_str = std::strchr(amount, ';')) {
	if (amount == note_str)
	  amount = NULL;

	*note_str++ = '\0';
	note_str = skip_ws(note_str);

	if (char * b = std::strchr(note_str, '['))
	  if (char * e = std::strchr(note_str, ']')) {
	    char buf[256];
	    std::strncpy(buf, b + 1, e - b);
	    buf[e - b] = '\0';

	    if (char * p = std::strchr(buf, '=')) {
	      *p++ = '\0';
	      if (! quick_parse_date(p, &xact->_date_eff))
		throw parse_error(path, linenum,
				  "Failed to parse effective date");
	    }

	    if (buf[0] && ! quick_parse_date(buf, &xact->_date))
	      throw parse_error(path, linenum, "Failed to parse date");
	  }

	xact->note = skip_ws(note_str);
      }

      if (amount) {
	bool in_quote	 = false;
	int  paren_depth = 0;
	for (char * q = amount; *q; q++) {
	  if (*q == '"') {
	      in_quote = ! in_quote;
	  }
	  else if (! in_quote) {
	    if (*q == '(')
	      paren_depth++;
	    else if (*q == ')')
	      paren_depth--;
	    else if (paren_depth == 0 && *q == '@') {
	      price = q;
	      break;
	    }
	  }
	}

	if (price) {
	  if (price == amount)
	    throw parse_error(path, linenum, "Cost specified without amount");

	  *price++ = '\0';
	  if (*price == '@') {
	    per_unit = false;
	    price++;
	  }
	  price = skip_ws(price);
	}
      }
    }
  }

  char * q = p + std::strlen(p) - 1;
  while (q >= p && std::isspace(*q))
    *q-- = '\0';

  if (*p == '[' || *p == '(') {
    xact->flags |= TRANSACTION_VIRTUAL;
    if (*p == '[')
      xact->flags |= TRANSACTION_BALANCE;
    p++;

    char * e = p + (std::strlen(p) - 1);
    assert(*e == ')' || *e == ']');
    *e = '\0';
  }

  if (account_aliases.size() > 0) {
    accounts_map::const_iterator i = account_aliases.find(p);
    if (i != account_aliases.end())
      xact->account = (*i).second;
  }
  if (! xact->account)
    xact->account = account->find_account(p);

  // If an amount (and optional price) were seen, parse them now
  if (amount) {
    xact->amount_expr = parse_amount(amount, xact->amount,
				     AMOUNT_PARSE_NO_REDUCE, *xact);
    if (price) {
      xact->cost = new amount_t;
      xact->cost_expr = parse_amount(price, *xact->cost,
				     AMOUNT_PARSE_NO_MIGRATE, *xact);
      if (per_unit) {
	*xact->cost *= xact->amount;
	*xact->cost = xact->cost->round(xact->cost->commodity().precision);
      }
    }
    xact->amount.reduce();
  }

  return xact.release();
}

bool parse_transactions(std::istream&	   in,
			account_t *	   account,
			entry_base_t&	   entry,
			const std::string& kind,
			unsigned long      beg_pos)
{
  static char line[MAX_LINE + 1];
  bool	      added = false;

  while (! in.eof() && (in.peek() == ' ' || in.peek() == '\t')) {
    in.getline(line, MAX_LINE);
    if (in.eof())
      break;
    beg_pos += std::strlen(line) + 1;
    linenum++;
    if (line[0] == ' ' || line[0] == '\t' || line[0] == '\r') {
      char * p = skip_ws(line);
      if (! *p || *p == '\r')
	break;
    }
    if (transaction_t * xact = parse_transaction(line, account)) {
      entry.add_transaction(xact);
      added = true;
    }
  }

  return added;
}

namespace {
  TIMER_DEF(parsing_total, "total parsing time");
  TIMER_DEF(entry_xacts,   "parsing transactions");
  TIMER_DEF(entry_details, "parsing entry details");
  TIMER_DEF(entry_date,    "parsing entry date");
}

entry_t * parse_entry(std::istream& in, char * line, account_t * master,
		      textual_parser_t& parser, unsigned long beg_pos)
{
  std::auto_ptr<entry_t> curr(new entry_t);

  // Parse the date

  TIMER_START(entry_date);

  char * next = next_element(line);

  if (char * p = std::strchr(line, '=')) {
    *p++ = '\0';
    if (! quick_parse_date(p, &curr->_date_eff))
      throw parse_error(path, linenum, "Failed to parse effective date");
  }

  if (! quick_parse_date(line, &curr->_date))
    throw parse_error(path, linenum, "Failed to parse date");

  TIMER_STOP(entry_date);

  // Parse the optional cleared flag: *

  TIMER_START(entry_details);

  transaction_t::state_t state = transaction_t::UNCLEARED;
  if (next) {
    switch (*next) {
    case '*':
      state = transaction_t::CLEARED;
      next = skip_ws(++next);
      break;
    case '!':
      state = transaction_t::PENDING;
      next = skip_ws(++next);
      break;
    }
  }

  // Parse the optional code: (TEXT)

  if (next && *next == '(') {
    if (char * p = std::strchr(next++, ')')) {
      *p++ = '\0';
      curr->code = next;
      next = skip_ws(p);
    }
  }

  // Parse the description text

  curr->payee = next ? next : "<Unspecified payee>";

  TIMER_STOP(entry_details);

  // Parse all of the transactions associated with this entry

  TIMER_START(entry_xacts);

  unsigned long end_pos;
  unsigned long beg_line = linenum;
  while (! in.eof() && (in.peek() == ' ' || in.peek() == '\t')) {
    line[0] = '\0';
    in.getline(line, MAX_LINE);
    if (in.eof() && line[0] == '\0')
      break;
    end_pos = beg_pos + std::strlen(line) + 1;

    linenum++;
    if (line[0] == ' ' || line[0] == '\t' || line[0] == '\r') {
      char * p = skip_ws(line);
      if (! *p || *p == '\r')
	break;
    }

    if (transaction_t * xact = parse_transaction(line, master)) {
      if (state != transaction_t::UNCLEARED &&
	  xact->state == transaction_t::UNCLEARED)
	xact->state = state;

      xact->beg_pos  = beg_pos;
      xact->beg_line = beg_line;
      xact->end_pos  = end_pos;
      xact->end_line = linenum;
      beg_pos = end_pos;
      curr->add_transaction(xact);
    }

    if (in.eof())
      break;
  }

  TIMER_STOP(entry_xacts);

  return curr.release();
}

template <typename T>
struct push_var {
  T& var;
  T prev;
  push_var(T& _var) : var(_var), prev(var) {}
  ~push_var() { var = prev; }
};

static inline void parse_symbol(char *& p, std::string& symbol)
{
  if (*p == '"') {
    char * q = std::strchr(p + 1, '"');
    if (! q)
      throw parse_error(path, linenum,
			"Quoted commodity symbol lacks closing quote");
    symbol = std::string(p + 1, 0, q - p - 1);
    p = q + 2;
  } else {
    char * q = next_element(p);
    symbol = p;
    if (q)
      p = q;
    else
      p += symbol.length();
  }
  if (symbol.empty())
    throw parse_error(path, linenum, "Failed to parse commodity");
}

bool textual_parser_t::test(std::istream& in) const
{
  char buf[5];

  in.read(buf, 5);
  if (std::strncmp(buf, "<?xml", 5) == 0) {
#if defined(HAVE_EXPAT) || defined(HAVE_XMLPARSE)
    throw parse_error(path, linenum, "Ledger file contains XML data, but format was not recognized");
#else
    throw parse_error(path, linenum, "Ledger file contains XML data, but no XML support present");
#endif
  }

  in.clear();
  in.seekg(0, std::ios::beg);
  assert(in.good());
  return true;
}

static void clock_out_from_timelog(const std::time_t when,
				   journal_t * journal)
{
  std::auto_ptr<entry_t> curr(new entry_t);
  curr->_date = when;
  curr->code  = "";
  curr->payee = last_desc;

  double diff = std::difftime(curr->_date, time_in);
  char   buf[32];
  std::sprintf(buf, "%lds", long(diff));
  amount_t amt;
  amt.parse(buf);

  transaction_t * xact
    = new transaction_t(last_account, amt, TRANSACTION_VIRTUAL);
  xact->state = transaction_t::CLEARED;
  curr->add_transaction(xact);

  if (! journal->add_entry(curr.get()))
    throw parse_error(path, linenum,
		      "Failed to record 'out' timelog entry");
  else
    curr.release();
}

unsigned int textual_parser_t::parse(std::istream&	 in,
				     config_t&           config,
				     journal_t *	 journal,
				     account_t *	 master,
				     const std::string * original_file)
{
  static bool   added_auto_entry_hook = false;
  static char   line[MAX_LINE + 1];
  char		c;
  unsigned int  count = 0;
  unsigned int  errors = 0;

  TIMER_START(parsing_total);

  std::list<account_t *> account_stack;
  auto_entry_finalizer_t auto_entry_finalizer(journal);

  if (! master)
    master = journal->master;

  account_stack.push_front(master);

  path	  = journal->sources.back();
  src_idx = journal->sources.size() - 1;
  linenum = 1;

  unsigned long beg_pos = in.tellg();
  unsigned long end_pos;
  unsigned long beg_line = linenum;
  while (in.good() && ! in.eof()) {
    try {
      in.getline(line, MAX_LINE);
      if (in.eof())
	break;
      linenum++;
      end_pos = beg_pos + std::strlen(line) + 1;

      switch (line[0]) {
      case '\0':
      case '\r':
	break;

      case ' ':
      case '\t': {
	char * p = skip_ws(line);
	if (*p && *p != '\r')
	  throw parse_error(path, linenum - 1, "Line begins with whitespace");
	break;
      }

#ifdef TIMELOG_SUPPORT
      case 'i':
      case 'I': {
	std::string date(line, 2, 19);

	char * p = skip_ws(line + 22);
	char * n = next_element(p, true);
	last_desc = n ? n : "";

	struct std::tm when;
	if (strptime(date.c_str(), "%Y/%m/%d %H:%M:%S", &when)) {
	  time_in      = std::mktime(&when);
	  last_account = account_stack.front()->find_account(p);
	} else {
	  last_account = NULL;
	  throw parse_error(path, linenum, "Cannot parse timelog entry date");
	}
	break;
      }

      case 'o':
      case 'O':
	if (last_account) {
	  std::string date(line, 2, 19);

	  char * p = skip_ws(line + 22);
	  if (last_desc.empty() && *p)
	    last_desc = p;

	  struct std::tm when;
	  if (strptime(date.c_str(), "%Y/%m/%d %H:%M:%S", &when)) {
	    clock_out_from_timelog(std::mktime(&when), journal);
	    count++;
	  } else {
	    throw parse_error(path, linenum, "Cannot parse timelog entry date");
	  }

	  last_account = NULL;
	}
	break;
#endif // TIMELOG_SUPPORT

      case 'D':	{		// a default commodity for "entry"
	amount_t amt;
	amt.parse(skip_ws(line + 1));
	commodity_t::default_commodity = &amt.commodity();
	break;
      }

      case 'A':		        // a default account for unbalanced xacts
	journal->basket =
	  account_stack.front()->find_account(skip_ws(line + 1));
	break;

      case 'C':			// a set of conversions
	if (char * p = std::strchr(line + 1, '=')) {
	  *p++ = '\0';
	  parse_conversion(line + 1, p);
	}
	break;

      case 'P': {		// a pricing entry
	std::time_t date;

	char * date_field = skip_ws(line + 1);
	char * time_field = next_element(date_field);
	if (! time_field) break;
	char * symbol_and_price = next_element(time_field);
	if (! symbol_and_price) break;

	char date_buffer[64];
	std::strcpy(date_buffer, date_field);
	date_buffer[std::strlen(date_field)] = ' ';
	std::strcpy(&date_buffer[std::strlen(date_field) + 1], time_field);

	struct std::tm when;
	if (strptime(date_buffer, "%Y/%m/%d %H:%M:%S", &when)) {
	  date = std::mktime(&when);
	} else {
	  throw parse_error(path, linenum, "Failed to parse date");
	}

	std::string symbol;
	amount_t    price;

	parse_symbol(symbol_and_price, symbol);
	price.parse(symbol_and_price);

	commodity_t * commodity = commodity_t::find_commodity(symbol, true);
	commodity->add_price(date, price);
	break;
      }

      case 'N': {			// don't download prices
	char * p = skip_ws(line + 1);
	std::string symbol;
	parse_symbol(p, symbol);

	commodity_t * commodity = commodity_t::find_commodity(symbol, true);
	commodity->flags |= COMMODITY_STYLE_NOMARKET;
	break;
      }

      case 'Y':                   // set the current year
	now_year = std::atoi(skip_ws(line + 1)) - 1900;
	break;

#ifdef TIMELOG_SUPPORT
      case 'h':
      case 'b':
#endif
      case ';':                   // a comment line
	break;

      case '-': {                 // option setting
	char * p = next_element(line);
	if (! p) {
	  p = std::strchr(line, '=');
	  if (p)
	    *p++ = '\0';
	}
	config.process_option(line + 2, p);
	break;
      }

      case '=': {		// automated entry
	if (! added_auto_entry_hook) {
	  journal->add_entry_finalizer(&auto_entry_finalizer);
	  added_auto_entry_hook = true;
	}

	auto_entry_t * ae = new auto_entry_t(skip_ws(line + 1));
	if (parse_transactions(in, account_stack.front(), *ae,
			       "automated", end_pos)) {
	  journal->auto_entries.push_back(ae);
	  ae->src_idx  = src_idx;
	  ae->beg_pos  = beg_pos;
	  ae->beg_line = beg_line;
	  ae->end_pos  = end_pos;
	  ae->end_line = linenum;
	}
	break;
      }

      case '~': {		// period entry
	period_entry_t * pe = new period_entry_t(skip_ws(line + 1));
	if (! pe->period)
	  throw parse_error(path, linenum,
			    std::string("Parsing time period '") + line + "'");

	if (parse_transactions(in, account_stack.front(), *pe,
			       "period", end_pos)) {
	  if (pe->finalize()) {
	    extend_entry_base(journal, *pe);
	    journal->period_entries.push_back(pe);
	    pe->src_idx	 = src_idx;
	    pe->beg_pos	 = beg_pos;
	    pe->beg_line = beg_line;
	    pe->end_pos	 = end_pos;
	    pe->end_line = linenum;
	  } else {
	    throw parse_error(path, linenum, "Period entry failed to balance");
	  }
	}
	break;
      }

      case '!': {                 // directive
	char * p = next_element(line);
	std::string word(line + 1);
	if (word == "include") {
	  push_var<std::string>	  save_path(path);
	  push_var<unsigned int>  save_src_idx(src_idx);
	  push_var<unsigned long> save_beg_pos(beg_pos);
	  push_var<unsigned long> save_end_pos(end_pos);
	  push_var<unsigned int>  save_linenum(linenum);

	  path = p;
	  if (path[0] != '/' && path[0] != '\\') {
	    std::string::size_type pos = save_path.prev.rfind('/');
	    if (pos == std::string::npos)
	      pos = save_path.prev.rfind('\\');
	    if (pos != std::string::npos)
	      path = std::string(save_path.prev, 0, pos + 1) + path;
	  }

	  DEBUG_PRINT("ledger.textual.include",
		      "Including path '" << path << "'");
	  count += parse_journal_file(path, config, journal,
				      account_stack.front());
	}
	else if (word == "account") {
	  account_t * acct;
	  acct = account_stack.front()->find_account(p);
	  account_stack.push_front(acct);
	}
	else if (word == "end") {
	  account_stack.pop_front();
	}
	else if (word == "alias") {
	  char * b = p;
	  if (char * e = std::strchr(b, '=')) {
	    char * z = e - 1;
	    while (std::isspace(*z))
	      *z-- = '\0';
	    *e++ = '\0';
	    e = skip_ws(e);

	    // Once we have an alias name (b) and the target account
	    // name (e), add a reference to the account in the
	    // `account_aliases' map, which is used by the transaction
	    // parser to resolve alias references.
	    account_t * acct = account_stack.front()->find_account(e);
	    std::pair<accounts_map::iterator, bool> result
	      = account_aliases.insert(accounts_pair(b, acct));
	    assert(result.second);
	  }
	}
	break;
      }

      default: {
	unsigned int first_line = linenum;
	unsigned long pos = end_pos;
	if (entry_t * entry =
	    parse_entry(in, line, account_stack.front(), *this, pos)) {
	  if (journal->add_entry(entry)) {
	    entry->src_idx  = src_idx;
	    entry->beg_pos  = beg_pos;
	    entry->beg_line = beg_line;
	    entry->end_pos  = end_pos;
	    entry->end_line = linenum;
	    count++;
	  } else {
	    print_entry(std::cerr, *entry);
	    delete entry;

	    std::string msgbuf;
	    std::ostringstream msg(msgbuf);
	    msg << "Entry above does not balance; remainder is: "
		<< entry_balance;
	    throw parse_error(path, first_line, msg.str());
	  }
	} else {
	  throw parse_error(path, first_line, "Failed to parse entry");
	}
	end_pos = pos;
	break;
      }
      }
    }
    catch (const parse_error& err) {
      std::cerr << "Error: " << err.what() << std::endl;
      errors++;
    }
    catch (const amount_error& err) {
      std::cerr << "Error: " << path << ", line " << (linenum - 1) << ": "
		<< err.what() << std::endl;;
      errors++;
    }
    catch (const error& err) {
      std::cerr << "Error: " << path << ", line " << (linenum - 1) << ": "
		<< err.what() << std::endl;;
      errors++;
    }
    beg_pos = end_pos;
  }

 done:
  if (last_account) {
    clock_out_from_timelog(now, journal);
    last_account = NULL;
  }

  if (added_auto_entry_hook)
    journal->remove_entry_finalizer(&auto_entry_finalizer);

  if (errors > 0)
    throw error(std::string("Errors parsing file '") + path + "'");

  TIMER_STOP(parsing_total);

  return count;
}

void write_textual_journal(journal_t& journal, std::string path,
			   item_handler<transaction_t>& formatter,
			   const std::string& write_hdr_format,
			   std::ostream& out)
{
  unsigned long index = 0;
  std::string   found;
  char          buf1[PATH_MAX];
  char          buf2[PATH_MAX];

#ifdef HAVE_REALPATH
  ::realpath(path.c_str(), buf1);
  for (strings_list::iterator i = journal.sources.begin();
       i != journal.sources.end();
       i++) {
    ::realpath((*i).c_str(), buf2);
    if (std::strcmp(buf1, buf2) == 0) {
      found = *i;
      break;
    }
    index++;
  }
#else
  for (strings_list::iterator i = journal.sources.begin();
       i != journal.sources.end();
       i++) {
    if (path == *i) {
      found = *i;
      break;
    }
    index++;
  }
#endif

  if (found.empty())
    throw error(std::string("Journal does not refer to file '") +
		path + "'");

  entries_list::iterator	el = journal.entries.begin();
  auto_entries_list::iterator	al = journal.auto_entries.begin();
  period_entries_list::iterator pl = journal.period_entries.begin();

  unsigned long pos = 0;

  format_t	hdr_fmt(write_hdr_format);
  std::ifstream in(found.c_str());

  while (! in.eof()) {
    entry_base_t * base = NULL;
    if (el != journal.entries.end() && pos == (*el)->beg_pos) {
      hdr_fmt.format(out, details_t(**el));
      base = *el++;
    }
    else if (al != journal.auto_entries.end() && pos == (*al)->beg_pos) {
      out << "= " << (*al)->predicate_string << '\n';
      base = *al++;
    }
    else if (pl != journal.period_entries.end() && pos == (*pl)->beg_pos) {
      out << "~ " << (*pl)->period_string << '\n';
      base = *pl++;
    }

    char c;
    if (base) {
      for (transactions_list::iterator x = base->transactions.begin();
	   x != base->transactions.end();
	   x++)
	if (! ((*x)->flags & TRANSACTION_AUTO)) {
	  transaction_xdata(**x).dflags |= TRANSACTION_TO_DISPLAY;
	  formatter(**x);
	}
      formatter.flush();

      while (pos < base->end_pos) {
	in.get(c);
	pos = in.tellg(); // pos++;
      }
    } else {
      in.get(c);
      pos = in.tellg(); // pos++;
      out.put(c);
    }
  }
}

} // namespace ledger