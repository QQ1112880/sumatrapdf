/* Copyright 2022 the SumatraPDF project authors (see AUTHORS file).
   License: Simplified BSD (see COPYING.BSD) */

#include "BaseUtil.h"
#include "StrFormat.h"

namespace fmt {

// formatting instruction
struct Inst {
    Type t;
    int argNo;           // <0 for strings that come from formatting string
    std::string_view sv; // if t is Type::FormatStr
};

struct Fmt {
    explicit Fmt(const char* fmt);

    bool Eval(const Arg** args, int nArgs);

    bool isOk = true; // true if mismatch between formatting instruction and args

    const char* format = nullptr;
    Inst instructions[32]; // 32 should be big enough for everybody
    int nInst = 0;

    int currArgNo = 0;
    int currPercArgNo = 0;
    str::Str res;

    char buf[256];
};

static Type typeFromChar(char c) {
    switch (c) {
        case 'c': // char
            return Type::Char;
        case 'd': // integer in base 10
            return Type::Int;
        case 'f': // float or double
            return Type::Float;
        case 's': // string or wstring
            return Type::Str;
        case 'v':
            return Type::Any;
    }
    CrashIf(true);
    return Type::None;
}

static void addFormatStr(Fmt& fmt, const char* s, size_t len) {
    if (len == 0) {
        return;
    }
    CrashIf(fmt.nInst >= dimof(fmt.instructions));
    fmt.instructions[fmt.nInst].t = Type::FormatStr;
    fmt.instructions[fmt.nInst].sv = {s, len};
    fmt.instructions[fmt.nInst].argNo = -1;
    ++fmt.nInst;
}

// parse: {$n}
static const char* parseArgDefPositional(Fmt& fmt, const char* s) {
    CrashIf(*s != '{');
    ++s;
    int n = 0;
    while (*s != '}') {
        // TODO: this could be more featurful
        CrashIf(!str::IsDigit(*s));
        n = n * 10 + (*s - '0');
        ++s;
    }
    fmt.instructions[fmt.nInst].t = Type::Any;
    fmt.instructions[fmt.nInst].argNo = n;
    ++fmt.nInst;
    return s + 1;
}

// parse: %[csfd]
static const char* parseArgDefPerc(Fmt& fmt, const char* s) {
    CrashIf(*s != '%');
    // TODO: more features
    fmt.instructions[fmt.nInst].t = typeFromChar(s[1]);
    fmt.instructions[fmt.nInst].argNo = fmt.currPercArgNo++;
    ++fmt.nInst;
    return s + 2;
}

static bool hasInstructionWithArgNo(Inst* insts, int nInst, int argNo) {
    for (int i = 0; i < nInst; i++) {
        if (insts[i].argNo == argNo) {
            return true;
        }
    }
    return false;
}

static bool validArgTypes(Type instType, Type argType) {
    if (instType == Type::Any || instType == Type::FormatStr) {
        return true;
    }
    if (instType == Type::Char) {
        return argType == Type::Char;
    }
    if (instType == Type::Int) {
        return argType == Type::Int;
    }
    if (instType == Type::Float) {
        return argType == Type::Float || argType == Type::Double;
    }
    if (instType == Type::Str) {
        return argType == Type::Str || argType == Type::WStr;
    }
    return false;
}

static bool ParseFormat(Fmt& o, const char* fmt) {
    o.format = fmt;
    o.nInst = 0;
    o.currPercArgNo = 0;
    o.currArgNo = 0;
    o.res.Reset();

    // parse formatting string, until a %$c or {$n}
    // %% is how we escape %, \{ is how we escape {
    const char* start = fmt;
    char c;
    while (*fmt) {
        c = *fmt;
        if ('\\' == c) {
            // handle \{
            if ('{' == fmt[1]) {
                addFormatStr(o, start, fmt - start);
                start = fmt + 1;
                fmt += 2; // skip '{'
                continue;
            }
            continue;
        }
        if ('{' == c) {
            addFormatStr(o, start, fmt - start);
            fmt = parseArgDefPositional(o, fmt);
            start = fmt;
            continue;
        }
        if ('%' == c) {
            // handle %%
            if ('%' == fmt[1]) {
                addFormatStr(o, start, fmt - start);
                start = fmt + 1;
                fmt += 2; // skip '%'
                continue;
            }
            addFormatStr(o, start, fmt - start);
            fmt = parseArgDefPerc(o, fmt);
            start = fmt;
            continue;
        }
        ++fmt;
    }
    addFormatStr(o, start, fmt - start);

    int maxArgNo = o.currArgNo;
    // check that arg numbers in {$n} makes sense
    for (int i = 0; i < o.nInst; i++) {
        if (o.instructions[i].t == Type::FormatStr) {
            continue;
        }
        if (o.instructions[i].argNo > maxArgNo) {
            maxArgNo = o.instructions[i].argNo;
        }
    }

    // instructions[i].argNo can be duplicate
    // (we can have positional arg like {0} multiple times
    // but must cover all space from 0..nArgsExpected
    for (int i = 0; i <= maxArgNo; i++) {
        bool isOk = hasInstructionWithArgNo(o.instructions, o.nInst, i);
        CrashIf(!isOk);
        if (!isOk) {
            return false;
        }
    }
    return true;
}

Fmt::Fmt(const char* fmt) {
    isOk = ParseFormat(*this, fmt);
}

bool Fmt::Eval(const Arg** args, int nArgs) {
    if (!isOk) {
        // if failed parsing format
        return false;
    }

    for (int n = 0; n < nInst; n++) {
        CrashIf(n >= dimof(instructions));
        CrashIf(n >= nInst);

        auto& inst = instructions[n];
        int argNo = inst.argNo;
        CrashIf(argNo >= nArgs);
        if (argNo >= nArgs) {
            isOk = false;
            return false;
        }

        if (inst.t == Type::FormatStr) {
            res.Append(inst.sv.data(), inst.sv.size());
            continue;
        }

        const Arg& arg = *args[argNo];
        isOk = validArgTypes(inst.t, arg.t);
        CrashIf(!isOk);
        if (!isOk) {
            return false;
        }

        switch (arg.t) {
            case Type::Char:
                res.AppendChar(arg.u.c);
                break;
            case Type::Int:
                // TODO: i64 is potentially bigger than int
                str::BufFmt(buf, dimof(buf), "%d", (int)arg.u.i);
                res.Append(buf);
                break;
            case Type::Float:
                // Note: %G, unlike %f, avoid trailing '0'
                str::BufFmt(buf, dimof(buf), "%G", arg.u.f);
                res.Append(buf);
                break;
            case Type::Double:
                // Note: %G, unlike %f, avoid trailing '0'
                str::BufFmt(buf, dimof(buf), "%G", arg.u.d);
                res.Append(buf);
                break;
            case Type::Str:
                res.Append(arg.u.s);
                break;
            case Type::WStr:
                char* s = ToUtf8Temp(arg.u.ws);
                res.Append(s);
                break;
        };
    }
    return true;
}

char* Format(const char* s, const Arg& a1, const Arg& a2, const Arg& a3, const Arg& a4, const Arg& a5, const Arg& a6) {
    const Arg* args[6];
    int nArgs = 0;
    args[nArgs++] = &a1;
    args[nArgs++] = &a2;
    args[nArgs++] = &a3;
    args[nArgs++] = &a4;
    args[nArgs++] = &a5;
    args[nArgs++] = &a6;
    CrashIf(nArgs > dimof(args));
    // arguments at the end could be empty
    while (nArgs >= 0 && args[nArgs - 1]->t == Type::None) {
        nArgs--;
    }

    if (nArgs == 0) {
        // TODO: verify that format has no references to args
        return str::Dup(s);
    }

    Fmt fmt(s);
    bool ok = fmt.Eval(args, nArgs);
    if (!ok) {
        return nullptr;
    }
    char* res = fmt.res.StealData();
    return res;
}

char* FormatTemp(const char* s, const Arg** args, int nArgs) {
    // arguments at the end could be empty
    while (nArgs >= 0 && args[nArgs - 1]->t == Type::None) {
        nArgs--;
    }

    if (nArgs == 0) {
        // TODO: verify that format has no references to args
        return (char*)s;
    }

    Fmt fmt(s);
    bool ok = fmt.Eval(args, nArgs);
    if (!ok) {
        return nullptr;
    }
    char* res = fmt.res.Get();
    size_t n = fmt.res.size();
    return str::DupTemp(res, n);
}

char* FormatTemp(const char* s, const Arg& a1, const Arg& a2, const Arg& a3, const Arg& a4, const Arg& a5,
                 const Arg& a6) {
    const Arg* args[6];
    int nArgs = 0;
    args[nArgs++] = &a1;
    args[nArgs++] = &a2;
    args[nArgs++] = &a3;
    args[nArgs++] = &a4;
    args[nArgs++] = &a5;
    args[nArgs++] = &a6;
    CrashIf(nArgs > dimof(args));
    return FormatTemp(s, args, nArgs);
}

char* FormatTemp(const char* s, const Arg a1) {
    const Arg* args[3] = {&a1};
    return FormatTemp(s, args, 1);
}

char* FormatTemp(const char* s, const Arg a1, const Arg a2) {
    const Arg* args[3] = {&a1, &a2};
    return FormatTemp(s, args, 2);
}

char* FormatTemp(const char* s, const Arg a1, const Arg a2, const Arg a3) {
    const Arg* args[3] = {&a1, &a2, &a3};
    return FormatTemp(s, args, 3);
}

} // namespace fmt
