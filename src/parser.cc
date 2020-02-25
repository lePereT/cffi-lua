#include <cstring>
#include <cctype>

#include <string>
#include <unordered_map>
#include <utility>
#include <stdexcept>

#include "parser.hh"
#include "ast.hh"

namespace parser {

/* define all keywords our subset of C understands */

/* stdint types might as well also be builtin... */
#define KEYWORDS KW(const), KW(enum), KW(extern), KW(struct), KW(typedef), \
    KW(signed), KW(unsigned), KW(volatile), KW(void), \
    \
    KW(bool), KW(char), KW(short), KW(int), KW(long), KW(float), KW(double), \
    \
    KW(int8_t), KW(uint8_t), KW(int16_t), KW(uint16_t), \
    KW(int32_t), KW(uint32_t), KW(int64_t), KW(uint64_t), \
    \
    KW(size_t), KW(ssize_t), KW(intptr_t), KW(uintptr_t), \
    KW(ptrdiff_t), KW(time_t)

/* primary keyword enum */

#define KW(x) KW_##x

/* a token is an int, single-char tokens are just their ascii */
enum c_token {
    TOK_INVALID = 257,

    TOK_INTEGER, TOK_NAME
};

enum c_keyword {
    KW_INVALID = 0,

    KEYWORDS
};

#undef KW

/* end primary keyword enum */

/* token strings */

#define KW(x) #x

static char const *tokens[] = { "<integer>", "<name>" };
static char const *keywords[] = {KEYWORDS};

#undef KW

/* end token strings */

/* lexer */

union lex_token_u {
    char c;
    signed int i;
    signed long l;
    signed long long ll;
    unsigned int u;
    unsigned long ul;
    unsigned long long ull;
    float f;
    double d;
};

enum c_int_literal {
    INT_INVALID = 0,

    INT_INT,
    INT_UINT,
    INT_LONG,
    INT_ULONG,
    INT_LLONG,
    INT_ULLONG
};

struct lex_token {
    int token = -1;
    int kw = KW_INVALID;
    std::string value_s;
    lex_token_u value{};
};

static thread_local std::unordered_map<std::string, int> keyword_map;

static void init_kwmap() {
    if (!keyword_map.empty()) {
        return;
    }
    for (int i = 1; i <= int(sizeof(keywords) / sizeof(keywords[0])); ++i) {
        keyword_map[keywords[i - 1]] = i;
    }
}

struct lex_state_error: public std::runtime_error {
    using std::runtime_error::runtime_error;
};

struct lex_state {
    lex_state() = delete;

    lex_state(
        char const *str, const char *estr
    ): stream(str), send(estr) {
        /* thread-local, initialize for parsing thread */
        init_kwmap();

        /* read first char */
        next_char();

        /* skip past potential UTF-8 BOM */
        if (current != 0xEF) {
            return;
        }
        next_char();
        if (current != 0xBB) {
            return;
        }
        next_char();
        if (current != 0xBF) {
            return;
        }
        next_char();
    }

    int get() {
        if (lahead.token >= 0) {
            t = std::move(lahead);
            lahead.token = -1;
            return t.token;
        }
        t.kw = KW_INVALID;
        return (t.token = lex(t));
    }

    int lookahead() {
        lahead.kw = KW_INVALID;
        return (lahead.token = lex(t));
    }

    void lex_error(std::string const &msg, int) {
        throw lex_state_error{msg};
    }

    void syntax_error(std::string const &msg) {
        lex_error(msg, t.token);
    }

private:
    bool is_newline(int c) {
        return (c == '\n') || (c == '\r');
    }

    void next_char() {
        if (stream == send) {
            current = '\0';
            return;
        }
        current = *(stream++);
    }

    void next_line() {
        int old = current;
        next_char();
        if (is_newline(current) && current != old) {
            next_char();
        }
        ++line_number; /* FIXME: handle overflow */
    }

    template<typename T>
    bool check_int_fits(unsigned long long val) {
        using U = unsigned long long;
        return (val <= U(std::numeric_limits<T>::max()));
    }

    /* this doesn't deal with stuff like negative values at all, that's
     * done in the expression parser as a regular unary expression and
     * is subject to the standard rules
     */
    int get_int_type(lex_token &tok, unsigned long long val, bool decimal) {
        bool unsig = false;
        int use_long = 0;
        if ((current | 32) == 'u') {
            unsig = true;
            next_char();
            if ((current | 32) == 'l') {
                ++use_long;
                next_char();
                if ((current | 32) == 'l') {
                    ++use_long;
                    next_char();
                }
            }
        } else if ((current | 32) == 'l') {
            ++use_long;
            next_char();
            if ((current | 32) == 'l') {
                ++use_long;
                next_char();
            }
            if ((current | 32) == 'u') {
                unsig = true;
            }
        }
        /* decimals still allow explicit unsigned, for others it's implicit */
        bool aus = (unsig || !decimal);
        switch (use_long) {
            case 0:
                /* no long suffix, can be any size */
                if (!unsig && check_int_fits<int>(val)) {
                    tok.value.i = static_cast<int>(val);
                    return INT_INT;
                } else if (aus && check_int_fits<unsigned int>(val)) {
                    tok.value.u = static_cast<unsigned int>(val);
                    return INT_UINT;
                } else if (!unsig && check_int_fits<long>(val)) {
                    tok.value.l = static_cast<long>(val);
                    return INT_LONG;
                } else if (aus && check_int_fits<unsigned long>(val)) {
                    tok.value.ul = static_cast<unsigned long>(val);
                    return INT_ULONG;
                } else if (!unsig && check_int_fits<long long>(val)) {
                    tok.value.ll = static_cast<long long>(val);
                    return INT_LLONG;
                } else if (aus) {
                    tok.value.ull = static_cast<unsigned long long>(val);
                    return INT_ULLONG;
                }
                break;
            case 1:
                /* l suffix */
                if (!unsig && check_int_fits<long>(val)) {
                    tok.value.l = static_cast<long>(val);
                    return INT_LONG;
                } else if (aus && check_int_fits<unsigned long>(val)) {
                    tok.value.ul = static_cast<unsigned long>(val);
                    return INT_ULONG;
                }
                break;
            case 2:
                /* ll suffix */
                if (!unsig && check_int_fits<long long>(val)) {
                    tok.value.ll = static_cast<long long>(val);
                    return INT_LLONG;
                } else if (aus) {
                    tok.value.ull = static_cast<unsigned long long>(val);
                    return INT_ULLONG;
                }
                break;
            default:
                break;
        }
        /* unsuffixed decimal and doesn't fit into signed long long,
         * or explicitly marked long and out of bounds
         */
        lex_error("value out of bounds", TOK_INTEGER);
        return INT_INVALID;
    }

    template<size_t base, typename F, typename G>
    void read_int_core(F &&digf, G &&convf, lex_token &tok) {
        char const *numbeg = (stream - 1);
        do {
            next_char();
        } while (digf(current));
        char const *numend = (stream - 1);
        size_t ndig = (numend - numbeg);
        /* decrement to last digit */
        --numend;
        /* go from the end */
        unsigned long long val = 0, mul = 1;
        do {
            /* standardize case */
            unsigned char dig = convf(current);
            val += dig * mul;
            mul *= base;
        } while (numend != numbeg);
        /* write type and value */
        tok.kw = get_int_type(tok, val, base == 10);
    }

    void read_integer(lex_token &tok) {
        if (current == '0') {
            next_char();
            if (!current || (
                ((current | 32) != 'x') && ((current | 32) != 'b') &&
                !(current >= '0' && current <= '7')
            )) {
                /* special case: value 0 */
                tok.value.i = 0;
                tok.kw = INT_INT;
                return;
            }
            if ((current | 32) == 'x') {
                /* hex */
                next_char();
                if (!isxdigit(current)) {
                    lex_error("malformed integer", TOK_INTEGER);
                    return;
                }
                read_int_core<16>(isxdigit, [](int dig) {
                    dig |= 32;
                    dig = (dig >= 'a') ? (dig - 'a' + 10) : (dig - '0');
                    return dig;
                }, tok);
            } else if ((current | 32) == 'b') {
                /* binary */
                next_char();
                if ((current != '0') && (current != '1')) {
                    lex_error("malformed integer", TOK_INTEGER);
                    return;
                }
                read_int_core<2>([](int cur) {
                    return (cur == '0') || (cur == '1');
                }, [](int dig) {
                    return (dig - '0');
                }, tok);
            } else {
                /* octal */
                read_int_core<8>([](int cur) {
                    return (cur >= '0') && (cur <= '7');
                }, [](int dig) {
                    return (dig - '0');
                }, tok);
            }
        } else {
            /* decimal */
            read_int_core<10>(isdigit, [](int dig) {
                return (dig - '0');
            }, tok);
        }
    }

    int lex(lex_token &tok) {
        for (;;) switch (current) {
            case '\0':
                return -1;
            case '\n':
            case '\r':
                next_line();
                continue;
            /* either comment or / */
            case '/': {
                next_char();
                if (current == '*') {
                    /* C style comment, TODO */
                } else if (current != '/') {
                    /* just / */
                    return '/';
                }
                /* C++ style comment */
                next_char();
                while (current && !is_newline(current)) {
                    next_char();
                }
                continue;
            }
            /* single-char tokens, keywords */
            default: {
                if (isspace(current)) {
                    next_char();
                    continue;
                } else if (isdigit(current)) {
                    p_buf.clear();
                    read_integer(tok);
                    return TOK_INTEGER;
                }
                if (current && (isalpha(current) || (current == '_'))) {
                    /* names, keywords */
                    /* what current pointed to */
                    char const *beg = (stream - 1);
                    /* keep reading until we readh non-matching char */
                    do {
                        next_char();
                    } while (current && (isalnum(current) || (current == '_')));
                    /* current is a non-matching char */
                    char const *end = (stream - 1);
                    std::string name{beg, end};
                    /* could be a keyword? */
                    auto kwit = keyword_map.find(name);
                    if (kwit != keyword_map.end()) {
                        tok.kw = kwit->second;
                    }
                    tok.value_s = std::move(name);
                    return TOK_NAME;
                }
                /* single-char token */
                int c = current;
                next_char();
                return c;
            }
        }
    }

    int current = -1;
    int line_number = 1;

    char const *stream;
    char const *send;

    std::string p_buf;

public:
    lex_token t, lahead;
};

/* parser */

static int parse_cv(lex_state &ls) {
    int quals = 0;

    for (;;) switch (ls.t.kw) {
        case KW_const:
            if (quals & ast::C_CV_CONST) {
                ls.syntax_error("duplicate const qualifier");
                break;
            }
            ls.get();
            quals |= ast::C_CV_CONST;
            break;
        case KW_volatile:
            if (quals & ast::C_CV_VOLATILE) {
                ls.syntax_error("duplicate volatile qualifier");
                break;
            }
            ls.get();
            quals |= ast::C_CV_VOLATILE;
            break;
        default:
            return quals;
    }

    return quals;
}

/* a bit naive for now, and builtins could be handled better (we don't care
 * about the real signatures, only about their sizes and signedness and so
 * on to provide to the codegen) but it's a start
 */
static ast::c_type parse_type(lex_state &ls) {
    /* left-side cv */
    int quals = parse_cv(ls);

    if (ls.t.kw == KW_signed || ls.t.kw == KW_unsigned) {
        quals |= (ls.t.kw == KW_signed) ? ast::C_CV_SIGNED : ast::C_CV_UNSIGNED;
        ls.get();
        switch (ls.t.kw) {
            case KW_char:
            case KW_short:
            case KW_int:
                /* restrict what can follow signed/unsigned */
                break;
            case KW_long:
                ls.lookahead();
                if (ls.lahead.kw == KW_double) {
                    ls.syntax_error("builtin integer type expected");
                }
                break;
            default:
                ls.syntax_error("builtin integer type expected");
                break;
        }
    }

    std::string tname;
    ast::c_builtin cbt = ast::C_BUILTIN_NOT;

    if (ls.t.token == TOK_NAME && !ls.t.kw) {
        /* a name but not a keyword, probably custom type */
        tname = ls.t.value_s;
        ls.get();
    } else switch (ls.t.kw) {
        /* may be a builtin type */
        case KW_void:
            cbt = ast::C_BUILTIN_VOID;
            goto btype;
        case KW_int8_t:
            cbt = ast::C_BUILTIN_INT8;
            goto btype;
        case KW_int16_t:
            cbt = ast::C_BUILTIN_INT16;
            goto btype;
        case KW_int32_t:
            cbt = ast::C_BUILTIN_INT32;
            goto btype;
        case KW_int64_t:
            cbt = ast::C_BUILTIN_INT64;
            goto btype;
        case KW_uint8_t:
            cbt = ast::C_BUILTIN_INT8;
            quals |= ast::C_CV_UNSIGNED;
            goto btype;
        case KW_uint16_t:
            cbt = ast::C_BUILTIN_INT16;
            quals |= ast::C_CV_UNSIGNED;
            goto btype;
        case KW_uint32_t:
            cbt = ast::C_BUILTIN_INT32;
            quals |= ast::C_CV_UNSIGNED;
            goto btype;
        case KW_uint64_t:
            cbt = ast::C_BUILTIN_INT64;
            quals |= ast::C_CV_UNSIGNED;
            goto btype;
        case KW_uintptr_t:
            cbt = ast::C_BUILTIN_INTPTR;
            quals |= ast::C_CV_UNSIGNED;
            goto btype;
        case KW_intptr_t:
            cbt = ast::C_BUILTIN_INTPTR;
            quals |= ast::C_CV_SIGNED;
            goto btype;
        case KW_ptrdiff_t:
            cbt = ast::C_BUILTIN_PTRDIFF;
            goto btype;
        case KW_ssize_t:
            cbt = ast::C_BUILTIN_SIZE;
            quals |= ast::C_CV_SIGNED;
            goto btype;
        case KW_size_t:
            cbt = ast::C_BUILTIN_SIZE;
            quals |= ast::C_CV_UNSIGNED;
            goto btype;
        case KW_time_t: cbt = ast::C_BUILTIN_TIME; goto btype;
        case KW_float:  cbt = ast::C_BUILTIN_FLOAT; goto btype;
        case KW_double: cbt = ast::C_BUILTIN_DOUBLE; goto btype;
        case KW_bool:   cbt = ast::C_BUILTIN_BOOL; goto btype;
        case KW_char:   cbt = ast::C_BUILTIN_CHAR; goto btype;
        case KW_short:  cbt = ast::C_BUILTIN_SHORT; goto btype;
        case KW_int:    cbt = ast::C_BUILTIN_INT;
        btype:
            tname = ls.t.value_s;
            ls.get();
            break;
        case KW_long:
            ls.get();
            if (ls.t.kw == KW_long) {
                cbt = ast::C_BUILTIN_LLONG;
                tname = "long long";
                ls.get();
            } else if (ls.t.kw == KW_int) {
                cbt = ast::C_BUILTIN_LONG;
                tname = "long";
                ls.get();
            } else if (ls.t.kw == KW_double) {
                cbt = ast::C_BUILTIN_LDOUBLE;
                tname = "long double";
                ls.get();
            } else {
                cbt = ast::C_BUILTIN_LONG;
                tname = "long";
            }
            break;
        default:
            ls.syntax_error("type name expected");
            break;
    }

    ast::c_type tp{tname, cbt, quals};
    for (;;) {
        /* right-side cv */
        tp.cv(parse_cv(ls));
        /* pointers plus their right side qualifiers */
        if (ls.t.token == '*') {
            ls.get();
            ast::c_type ptp{std::move(tp), parse_cv(ls)};
            using CT = ast::c_type;
            tp.~CT();
            new (&tp) ast::c_type{std::move(ptp)};
        } else {
            break;
        }
    }

    return std::move(tp);
}

static void parse_decl(lex_state &ls) {
    switch (ls.t.kw) {
        case KW_typedef:
            //parse_typedef(ls);
            return;
        case KW_struct:
            //parse_struct(ls);
            return;
        case KW_enum:
            //parse_enum(ls);
            return;
        case KW_extern:
            /* may precede any declaration without changing its behavior */
            ls.get();
            break;
    }

    auto tp = parse_type(ls);
    std::string dname;

    if (ls.t.token != TOK_NAME) {
        ls.syntax_error("name expected");
    }
    dname = ls.t.value_s;
    ls.get();

    if (ls.t.token == ';') {
        ast::add_decl(new ast::c_variable{std::move(dname), std::move(tp)});
        return;
    } else if (ls.t.token != '(') {
        ls.syntax_error("';' expected");
        return;
    }

    ls.get();

    std::vector<ast::c_param> params;

    for (;;) {
        auto pt = parse_type(ls);
        if (ls.t.token == ',') {
            /* unnamed param */
            ls.get();
            continue;
        }
        if (ls.t.token != TOK_NAME) {
            ls.syntax_error("parameter name expected");
        }
        params.emplace_back(ls.t.value_s, std::move(pt));
        ls.get();
        if (ls.t.token == ')') {
            ls.get();
            break;
        } else if (ls.t.token != ',') {
            ls.syntax_error("')' expected");
        }
        ls.get();
    }

    ast::add_decl(new ast::c_function{
        std::move(dname), std::move(tp), std::move(params)
    });
}

static void parse_decls(lex_state &ls) {
    while (ls.t.token >= 0) {
        if (ls.t.token == ';') {
            /* empty statement */
            ls.get();
            continue;
        }
        parse_decl(ls);
        if (!ls.t.token) {
            break;
        }
        if (ls.t.token != ';') {
            ls.syntax_error("';' expected");
        }
        ls.get();
    }
}

void parse(std::string const &input) {
    lex_state ls{input.c_str(), input.c_str() + input.size()};

    /* read first token */
    ls.get();

    parse_decls(ls);
}

} /* namespace parser */
