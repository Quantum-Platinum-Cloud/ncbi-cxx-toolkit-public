/* $Id$
 * ===========================================================================
 *
 *                            PUBLIC DOMAIN NOTICE
 *               National Center for Biotechnology Information
 *
 *  This software/database is a "United States Government Work" under the
 *  terms of the United States Copyright Act.  It was written as part of
 *  the author's official duties as a United States Government employee and
 *  thus cannot be copyrighted.  This software/database is freely available
 *  to the public for use. The National Library of Medicine and the U.S.
 *  Government have not placed any restriction on its use or reproduction.
 *
 *  Although all reasonable efforts have been taken to ensure the accuracy
 *  and reliability of the software and data, the NLM and the U.S.
 *  Government do not and cannot warrant the performance or results that
 *  may be obtained by using this software or data. The NLM and the U.S.
 *  Government disclaim all warranties, express or implied, including
 *  warranties of performance, merchantability or fitness for any particular
 *  purpose.
 *
 *  Please cite the author in any work or product based on this material.
 *
 * ===========================================================================
 *
 * Author:  Denis Vakatov
 *
 * File Description:
 *   NCBI C++ stream class wrappers
 *   Triggering between "new" and "old" C++ stream libraries
 *
 */

#include <ncbi_pch.hpp>
#include <corelib/ncbistd.hpp>
#include <corelib/ncbisys.hpp>
#include <corelib/ncbistre.hpp>
#include <corelib/stream_utils.hpp>
#if defined(NCBI_OS_UNIX)
#  include <unistd.h>
#endif


BEGIN_NCBI_SCOPE


#if defined(NCBI_OS_MSWIN) && defined(_UNICODE)
wstring ncbi_Utf8ToWstring(const char *utf8)
{
    return _T_XSTRING(utf8);
}
#endif


CNcbiIstream& NcbiGetline(CNcbiIstream& is, string& str, const string& delims,
                          SIZE_TYPE* count)
{
    str.erase();

    IOS_BASE::fmtflags f = is.flags();
    is.unsetf(IOS_BASE::skipws);
#ifdef NO_PUBSYNC
    if ( !is.ipfx(1) ) {
        is.flags(f);
        is.setstate(NcbiFailbit);
        return is;
    }
#else
    CNcbiIstream::sentry s(is);
    if ( !s ) {
        is.flags(f);
        is.setstate(NcbiFailbit);
        return is;
    }
#endif //NO_PUBSYNC
    _ASSERT( is.good() );

    char buf[1024];
    SIZE_TYPE pos = 0;
    SIZE_TYPE size = 0;
    SIZE_TYPE max_size = str.max_size();
    SIZE_TYPE delim_count = 0;
    IOS_BASE::iostate iostate = NcbiGoodbit/*0*/;
    for (;;) {
        CT_INT_TYPE ch = is.rdbuf()->sbumpc();
        if ( CT_EQ_INT_TYPE(ch, CT_EOF) ) {
            iostate = NcbiEofbit;
            break;
        }
        SIZE_TYPE delim_pos = delims.find(CT_TO_CHAR_TYPE(ch));
        if (delim_pos != NPOS) {
            // Special case -- if two different delimiters are back to
            // back and in the same order as in delims, treat them as
            // a single delimiter (necessary for correct handling of
            // DOS/MAC-style CR/LF endings).
            ch = is.rdbuf()->sgetc();
            if (!CT_EQ_INT_TYPE(ch, CT_EOF)
                &&  delims.find(CT_TO_CHAR_TYPE(ch), delim_pos + 1) != NPOS) {
                is.rdbuf()->sbumpc();
                delim_count = 2;
            } else {
                delim_count = 1;
            }
            break;
        }
        if (size == max_size) {
            CT_INT_TYPE bk = is.rdbuf()->sungetc();
            iostate = CT_EQ_INT_TYPE(bk, ch) ? NcbiFailbit : NcbiBadbit;
            break;
        }

        buf[pos++] = CT_TO_CHAR_TYPE(ch);
        if (pos == sizeof(buf)) {
            str.append(buf, pos);
            pos  = 0;
        }
        size++;
    }
    if (pos > 0)
        str.append(buf, pos);
    if (count != NULL)
        *count = size + delim_count;

#ifdef NO_PUBSYNC
    is.isfx();
#endif //NO_PUBSYNC
    is.flags(f);
    if (iostate) {
        if (iostate == NcbiEofbit  &&  str.empty())
            iostate |= NcbiFailbit;
        is.clear(iostate);
    }
    return is;
}


extern CNcbiIstream& NcbiGetline(CNcbiIstream& is, string& str, char delim,
                                 SIZE_TYPE* count)
{
#if   defined(NCBI_USE_OLD_IOSTREAM)
    return NcbiGetline(is, str, string(1, delim), count);
#else
    str.erase();

    if ( !is.good() ) {
        is.setstate(NcbiFailbit);
        return is;
    }

    char buf[1024];
    SIZE_TYPE size = 0;
    SIZE_TYPE max_size = str.max_size();
    do {
        CT_INT_TYPE nextc = is.get();
        if (CT_EQ_INT_TYPE(nextc, CT_EOF) 
            ||  CT_EQ_INT_TYPE(nextc, CT_TO_INT_TYPE(delim))) {
            ++size;
            break;
        }
        if ( !is.unget() )
            break;
        if (size == max_size) {
            is.clear(NcbiFailbit);
            break;
        }
        SIZE_TYPE n = max_size - size;
        is.get(buf, n < sizeof(buf) ? n : sizeof(buf), delim);
        n = (size_t) is.gcount();
        str.append(buf, n);
        size += n;
        _ASSERT(size == str.length());
    } while ( is.good() );
#endif

    if (is.rdstate() == NcbiEofbit  &&  str.empty())
        is.setstate(NcbiFailbit);
    if (count != NULL)
        *count = size;
    return is;
}


// Platform-specific EndOfLine
const char* Endl(void)
{
#if defined(NCBI_OS_MSWIN)
    static const char s_Endl[] = "\r\n";
#else /* assume UNIX-like EOLs */
    static const char s_Endl[] = "\n";
#endif
    return s_Endl;
}


// Get a line taking into account platform-specific of End-Of-Line
CNcbiIstream& NcbiGetlineEOL(CNcbiIstream& is, string& str, SIZE_TYPE* count)
{
#if   defined(NCBI_OS_MSWIN)
    NcbiGetline(is, str, '\n', count);
    if (!str.empty()  &&  str[str.length() - 1] == '\r')
        str.resize(str.length() - 1);
#elif defined(NCBI_OS_DARWIN)
    NcbiGetline(is, str, "\r\n", count);
#else /* assume UNIX-like EOLs */
    NcbiGetline(is, str, '\n', count);
#endif //NCBI_OS_...
    return is;
}


bool NcbiStreamCopy(CNcbiOstream& os, CNcbiIstream& is)
{
    if (!os.good()  ||  is.bad())
        return false;
    if (CT_EQ_INT_TYPE(is.peek(), CT_EOF)) {
        // NB: C++ Std says nothing about eofbit (27.6.1.3.27)
        _ASSERT(!is.good());
        return !is.bad();
    }
    os << is.rdbuf();
    return os.good()  &&  os.flush() ? true : false;
}


void NcbiStreamCopyThrow(CNcbiOstream& os, CNcbiIstream& is)
{
    bool success = false;
    try {
        success = NcbiStreamCopy(os, is);
    }
    NCBI_CATCH_ALL("NcbiStreamCopy()");
    if (!success) {
        NCBI_THROW(CCoreException, eCore, "NcbiStreamCopy() failed");
    }
}


void NcbiStreamCopyHead(CNcbiOstream& os, CNcbiIstream& is, SIZE_TYPE count)
{
    if ( !is.good() ) {
        is.setstate(NcbiFailbit);
        NCBI_THROW(CCoreException, eCore, "Input stream already in bad state");
    }
    if ( os.bad() ) {
        os.setstate(NcbiFailbit);
        NCBI_THROW(CCoreException, eCore,
                   "Output stream already in bad state");
    }
    if (CT_EQ_INT_TYPE(is.peek(), CT_EOF)) {
        // NB: C++ Std says nothing about eofbit (27.6.1.3.27)
        _ASSERT( !is.good() );
        if (is.bad()) {
            NCBI_THROW(CCoreException, eCore,
                       "Input stream already in bad state (at EOF)");
        }
    }
    char      buffer[16384];
    SIZE_TYPE ninstream = count, ninbuffer = 0;
    auto      outbuf = os.rdbuf();
    while (ninstream > 0  ||  ninbuffer > 0) {
        _ASSERT(ninbuffer < sizeof(buffer));
        auto nwanted  = min(sizeof(buffer) - ninbuffer, ninstream);
        streamsize nread = 0;
        if (nwanted > 0) {
            is.read(buffer + ninbuffer, nwanted);
            nread = is.gcount();
            if ( !is.good() ) {
                is.setstate(NcbiFailbit);
                ninstream = nread;
            }
        }
        auto nwritten = outbuf->sputn(buffer, nread + ninbuffer);
        if ( os.bad()  ||  nwritten == 0) {
            os.setstate(NcbiFailbit);
            NCBI_THROW(CCoreException, eCore, "Write error");
        }
        _ASSERT(static_cast<SIZE_TYPE>(nwritten) <= nread + ninbuffer);
        ninbuffer = nread + ninbuffer - nwritten;
        if (ninbuffer > 0) {
            memmove(buffer, buffer + nwritten, ninbuffer);
        }
        _ASSERT(static_cast<SIZE_TYPE>(nread) <= ninstream);
        ninstream -= nread;
    }
    if ( !os.flush() ) {
        NCBI_THROW(CCoreException, eCore, "Flush error");
    }
    // Deferred to ensure writing as much as possible when reading fails
    // and writing sometimes yields leftovers.
    if ( is.bad() ) {
        NCBI_THROW(CCoreException, eCore, "Read error");
    }
}


size_t NcbiStreamToString(string* str, CNcbiIstream& is, size_t pos)
{
    if (!is.good()) {
        // Can't extract anything
        if (str)
            str->resize(pos);
        is.setstate(NcbiFailbit);
        return 0;
    }

    char   buf[5120];
    size_t buf_size = sizeof(buf);
    size_t str_size;

    if (str) {
        str_size = pos;
        if (str->size() < str_size + buf_size)
            str->resize(str_size + buf_size);
    } else
        str_size = pos = 0;

    do {
        try {
            is.read(str ? &(*str)[str_size] : buf, buf_size);
        } catch (...) {
            if (str)
                str->resize(str_size);
            throw;
        }
        streamsize count = is.gcount();
        str_size += (size_t) count;
        if (str) {
            if ((size_t) count == buf_size) {
                if (buf_size < (1UL << 20))
                    buf_size <<= 1;
                str->resize(str_size + buf_size);
            } else
                _ASSERT(!is.good());
        }
    } while (is.good());

    _ASSERT(str_size >= pos);
    if (str)
        str->resize(str_size);

    if (!(str_size -= pos)) {
        // Nothing extracted
        is.setstate(NcbiFailbit);
        return 0;
    }

    // NB: istream::read() sets both bits at EOF (27.6.1.3.28)
    IOS_BASE::iostate iostate = is.rdstate();
    if (iostate != (NcbiFailbit | NcbiEofbit))
        return 0;
    is.clear(iostate & ~NcbiFailbit);
    return str_size;
}


bool NcbiStreamCompare(CNcbiIstream& is1, CNcbiIstream& is2)
{
    while (is1 && is2) {
        char c1 = (char)is1.get();
        char c2 = (char)is2.get();
        if (c1 != c2) {
            return false;
        }
    }
    return is1.eof() && is2.eof();
}


static inline
char x_GetChar(CNcbiIstream& is, ECompareTextMode mode,
               char* buf, size_t buf_size, char*& pos, size_t& sizeleft)
{
    char c;
    do {
        if ( !sizeleft ) {
            is.read(buf, buf_size);
            sizeleft = (size_t) is.gcount();
            pos = buf;
        }
        if (sizeleft > 0) {
            c = *pos++;
            --sizeleft;
        } else {
            return '\0';
        }
    } while ( (mode == eCompareText_IgnoreEol
               &&  (c == '\n'  ||  c == '\r'))  ||
              (mode == eCompareText_IgnoreWhiteSpace
               &&  isspace((unsigned char) c)) );
    return c;
}


bool NcbiStreamCompareText(CNcbiIstream& is1, CNcbiIstream& is2,
                           ECompareTextMode mode, size_t buf_size)
{
    if ( !buf_size ) {
        buf_size = 4 * 1024;
    }
    char*  buf1  = new char[buf_size];
    char*  buf2  = new char[buf_size];
    size_t size1 = 0, size2 = 0;
    char   *pos1 = 0, *pos2 = 0;
    bool   equal = true;
    do {
        char c1 = x_GetChar(is1, mode, buf1, buf_size, pos1, size1);
        char c2 = x_GetChar(is2, mode, buf2, buf_size, pos2, size2);
        equal = (c1 == c2);
        if (!c1  ||  !c2) {
            break;
        }
    } while ( equal );
    delete[] buf1;
    delete[] buf2;
    return equal  &&  is1.eof()  &&  is2.eof();
}


bool NcbiStreamCompareText(CNcbiIstream& is, const string& str,
                           ECompareTextMode mode, size_t buf_size)
{
    CNcbiIstrstream istr(str);
    return NcbiStreamCompareText(is, istr, mode, buf_size);
}


CNcbiOstrstreamToString::operator string(void) const
{
#ifdef NCBI_SHUN_OSTRSTREAM
    return m_Out.str();
#else
    SIZE_TYPE len = (SIZE_TYPE) m_Out.pcount();
    if ( !len ) {
        return string();
    }
    const char* str = m_Out.str();
    m_Out.freeze(false);
    return string(str, len);
#endif
}


CNcbiOstream& operator<<(CNcbiOstream& out, const CNcbiOstrstreamToString& s)
{
#ifdef NCBI_SHUN_OSTRSTREAM
    out << s.m_Out.str();
#else
    SIZE_TYPE len = (SIZE_TYPE) s.m_Out.pcount();
    if ( len ) {
        const char* str = s.m_Out.str();
        s.m_Out.freeze(false);
        out.write(str, len);
    }
#endif
    return out;
}


CNcbiOstream& operator<<(CNcbiOstream& out, CUpcaseStringConverter s)
{
    ITERATE ( string, c, s.m_String ) {
        out.put(char(toupper((unsigned char)(*c))));
    }
    return out;
}


CNcbiOstream& operator<<(CNcbiOstream& out, CLocaseStringConverter s)
{
    ITERATE ( string, c, s.m_String ) {
        out.put(char(tolower((unsigned char)(*c))));
    }
    return out;
}


CNcbiOstream& operator<<(CNcbiOstream& out, CUpcaseCharPtrConverter s)
{
    for ( const char* c = s.m_String; *c; ++c ) {
        out.put(char(toupper((unsigned char)(*c))));
    }
    return out;
}


CNcbiOstream& operator<<(CNcbiOstream& out, CLocaseCharPtrConverter s)
{
    for ( const char* c = s.m_String; *c; ++c ) {
        out.put(char(tolower((unsigned char)(*c))));
    }
    return out;
}


#ifdef NCBI_COMPILER_MSVC
#  if _MSC_VER >= 1200  &&  _MSC_VER < 1300
CNcbiOstream& operator<<(CNcbiOstream& out, __int64 val)
{
    return (out << NStr::Int8ToString(val));
}
#  endif
#endif


string Printable(char c)
{
    static const char kHex[] = "0123456789ABCDEF";

    string s;
    switch ( c ) {
    case '\0':  s += "\\0";   break;
    case '\t':  s += "\\t";   break;
    case '\v':  s += "\\v";   break;
    case '\b':  s += "\\b";   break;
    case '\r':  s += "\\r";   break;
    case '\f':  s += "\\f";   break;
    case '\a':  s += "\\a";   break;
    case '\n':  s += "\\n";   break;
    case '\\':  s += "\\\\";  break;
    case '\'':  s += "\\'";   break;
    case '"':   s += "\\\"";  break;
    default:
        if ( !isprint((unsigned char) c) ) {
            s += "\\x";
            s += kHex[(unsigned char) c / 16];
            s += kHex[(unsigned char) c % 16];
        } else
            s += c;
        break;
    }
    return s;
}


static inline
bool s_IsQuoted(char c)
{
    return (c == '\t'  ||   c == '\v'  ||  c == '\b'  ||
            c == '\r'  ||   c == '\f'  ||  c == '\a'  ||
            c == '\n'  ||   c == '\\'  ||  c == '\''  ||
            c == '"'   ||  !isprint((unsigned char) c) ? true : false);
}


static inline
void s_WritePrintable(CNcbiOstream& out, char c, char n)
{
    switch ( c ) {
    case '\t':  out.write("\\t",  2);  return;
    case '\v':  out.write("\\v",  2);  return;
    case '\b':  out.write("\\b",  2);  return;
    case '\r':  out.write("\\r",  2);  return;
    case '\f':  out.write("\\f",  2);  return;
    case '\a':  out.write("\\a",  2);  return;
    case '\n':  out.write("\\n",  2);  return;
    case '\\':  out.write("\\\\", 2);  return;
    case '\'':  out.write("\\'",  2);  return;
    case '"':   out.write("\\\"", 2);  return;
    default:
        if ( isprint((unsigned char) c) ) {
            out.put(c);
            return;
        }
        break;
    }

    bool full = !s_IsQuoted(n)  &&  '0' <= n  &&  n <= '7' ? true : false;
    unsigned char v;
    char octal[4];
    int k = 1;

    *octal = '\\';
    v = (unsigned char)((unsigned char) c >> 6);
    if (v  ||  full) {
        octal[k++] = char('0' + v);
        full = true;
    }
    v = ((unsigned char) c >> 3) & 7;
    if (v  ||  full) {
        octal[k++] = char('0' + v);
    }
    v = (unsigned char) c & 7;
    octal[k++] = char('0' + v);
    out.write(octal, k);
}


CNcbiOstream& operator<<(CNcbiOstream& out, CPrintableStringConverter s)
{
    size_t size = s.m_String.size();
    if (size) {
        const char* data = s.m_String.data();
        for (size_t i = 0;  i < size - 1;  ++i) {
            s_WritePrintable(out, data[i], data[i + 1]);
        }
        s_WritePrintable(out, data[size - 1], '\0');
    }
    return out;
}


CNcbiOstream& operator<<(CNcbiOstream& out, CPrintableCharPtrConverter s)
{
    const char* p = s.m_String;
    char        c = *p;
    while (c) {
        char n = *++p;
        s_WritePrintable(out, c, n);
        c = n;
    }
    return out;
}


#if defined(NCBI_COMPILER_WORKSHOP)
// We have to use two #if's here because KAI C++ cannot handle #if foo == bar
#  if (NCBI_COMPILER_VERSION == 530)
// The version that ships with the compiler is buggy.
// Here's a working (and simpler!) one.
template<>
istream& istream::read(char *s, streamsize n)
{
    sentry ipfx(*this, 1);

    try {
        if (rdbuf()->sgetc() == traits_type::eof()) {
            // Workaround for bug in sgetn.  *SIGH*.
            __chcount = 0;
            setstate(eofbit);
            return *this;
        }
        __chcount = rdbuf()->sgetn(s, n);
        if (__chcount == 0) {
            setstate(eofbit);
        } else if (__chcount < n) {
            setstate(eofbit | failbit);
        } else if (!ipfx) {
            setstate(failbit);
        } 
    } catch (...) {
        setstate(badbit | failbit);
    }

    return *this;
}
#  endif  /* NCBI_COMPILER_VERSION == 530 */
#endif  /* NCBI_COMPILER_WORKSHOP */


EEncodingForm ReadIntoUtf8(
    CNcbiIstream&     input,
    CStringUTF8*      result,
    EEncodingForm     ef             /* = eEncodingForm_Unknown */,
    EReadUnknownNoBOM what_if_no_bom /* = eNoBOM_GuessEncoding  */
)
{
    EEncodingForm ef_bom = eEncodingForm_Unknown;
    result->erase();
    if (!input.good()) {
        return ef_bom;
    }

    const int buf_size = 4096;//2048;//256;
    char tmp[buf_size+2];
    Uint2* us = reinterpret_cast<Uint2*>(tmp);

    // check for Byte Order Mark
    const int bom_max = 4;
    memset(tmp,0,bom_max);
    input.read(tmp,bom_max);
    int n = (int)input.gcount();
    {
        int bom_len=0;
        Uchar* uc = reinterpret_cast<Uchar*>(tmp);
        if (n >= 3  &&  uc[0] == 0xEF  &&  uc[1] == 0xBB  &&  uc[2] == 0xBF) {
            ef_bom = eEncodingForm_Utf8;
            uc[0] = uc[3];
            bom_len=3;
        }
        else if (n >= 2 && (us[0] == 0xFEFF || us[0] == 0xFFFE)) {
            if (us[0] == 0xFEFF) {
                ef_bom = eEncodingForm_Utf16Native;
            } else {
                ef_bom = eEncodingForm_Utf16Foreign;
            }
            us[0] = us[1];
            bom_len=2;
        }
        if (ef == eEncodingForm_Unknown  ||  ef == ef_bom) {
            ef = ef_bom;
            n -= bom_len;
        }
        // else proceed at user's risk
    }

    // keep reading
    while (n != 0  ||  (input.good()  &&  !input.eof())) {

        if (n == 0) {
            input.read(tmp, buf_size);
            n = (int) input.gcount();
            result->reserve(max(result->capacity(), result->size() + n));
        }
        tmp[n] = '\0';

        switch (ef) {
        case eEncodingForm_Utf16Foreign:
            {
                char buf[buf_size];
                NcbiSys_swab(tmp, buf, n);
                memcpy(tmp, buf, n);
            }
            // no break here
        case eEncodingForm_Utf16Native:
            {
                Uint2* u = us;
#if 0
                for (n = n/2;  n--;  ++u) {
                    result->Append(*u);
                }
#else
                *result += CUtf8::AsUTF8(u, n/2);
#endif
            }
            break;
        case eEncodingForm_ISO8859_1:
            //result->Append(tmp,eEncoding_ISO8859_1);
            *result += CUtf8::AsUTF8(tmp,eEncoding_ISO8859_1);
            break;
        case eEncodingForm_Windows_1252:
            //result->Append(tmp,eEncoding_Windows_1252);
            *result += CUtf8::AsUTF8(tmp,eEncoding_Windows_1252);
            break;
        case eEncodingForm_Utf8:
            //result->Append(tmp,eEncoding_UTF8);   
            result->append(tmp,n);
            break;
        default:
            if (what_if_no_bom == eNoBOM_GuessEncoding) {
                if (n == bom_max) {
                    input.read(tmp + n, buf_size - n);
                    n += (int) input.gcount();
                    result->reserve(max(result->capacity(), result->size() + n));
                }
                tmp[n] = '\0';
                EEncoding enc = CUtf8::GuessEncoding(tmp);
                switch (enc) {
                default:
                case eEncoding_Unknown:
                    if (CUtf8::GetValidBytesCount( CTempString(tmp, n)) != 0) {
                        ef = eEncodingForm_Utf8;
                        //result->Append(tmp, enc);
                        *result += CUtf8::AsUTF8(tmp, enc);
                    }
                    else {
                        NCBI_THROW(CCoreException, eCore,
                                "ReadIntoUtf8: cannot guess text encoding");
                    }
                    break;
                case eEncoding_UTF8:
                    ef = eEncodingForm_Utf8;
                    // no break here
                case eEncoding_Ascii:
                case eEncoding_ISO8859_1:
                case eEncoding_Windows_1252:
                    //result->Append(tmp, enc);
                    *result += CUtf8::AsUTF8(tmp,enc);
                    break;
                }
            } else {
                //result->Append(tmp, eEncoding_UTF8);
                result->append(tmp, n);
            }
            break;
        }
        n = 0;
    }
    return ef_bom;
}


EEncodingForm GetTextEncodingForm(CNcbiIstream& input,
                                  EBOMDiscard   discard_bom)
{
    EEncodingForm ef = eEncodingForm_Unknown;
    if (input.good()) {
        const int bom_max = 4;
        char tmp[bom_max];
        memset(tmp, 0, bom_max);
        Uint2* us = reinterpret_cast<Uint2*>(tmp);
        Uchar* uc = reinterpret_cast<Uchar*>(tmp);
        input.get(tmp[0]);
        int n = (int) input.gcount();
        if (n == 1  &&  (uc[0] == 0xEF  ||  uc[0] == 0xFE  ||  uc[0] == 0xFF)){
            input.get(tmp[1]);
            if (input.gcount() == 1) {
                ++n;
                if (us[0] == 0xFEFF) {
                    ef = eEncodingForm_Utf16Native;
                } else if (us[0] == 0xFFFE) {
                    ef = eEncodingForm_Utf16Foreign;
                } else if (uc[1] == 0xBB) {
                    input.get(tmp[2]);
                    if (input.gcount() == 1) {
                        ++n;
                        if (uc[2] == 0xBF) {
                            ef = eEncodingForm_Utf8;
                        }
                    }
                }
            }
        }
        if (ef == eEncodingForm_Unknown) {
            if (n > 1) {
                CStreamUtils::Pushback(input, tmp, n);
            } else if (n == 1) {
                input.unget();
            }
        } else {
            if (discard_bom == eBOM_Keep) {
                CStreamUtils::Pushback(input, tmp, n);
            }
        }
    }
    return ef;
}

CNcbiOstream& operator<< (CNcbiOstream& str, const CByteOrderMark&  bom)
{
    switch (bom.GetEncodingForm()) {
    /// Stream has no BOM.
    default:
    case eEncodingForm_Unknown:
    case eEncodingForm_ISO8859_1:
    case eEncodingForm_Windows_1252:
        break;
    case eEncodingForm_Utf8:
        str << Uint1(0xEF) << Uint1(0xBB) << Uint1(0xBF);
        break;
    case eEncodingForm_Utf16Native:
#ifdef WORDS_BIGENDIAN 
        str << Uint1(0xFE) << Uint1(0xFF);
#else
        str << Uint1(0xFF) << Uint1(0xFE);
#endif
        break;
    case eEncodingForm_Utf16Foreign:
#ifdef WORDS_BIGENDIAN 
        str << Uint1(0xFF) << Uint1(0xFE);
#else
        str << Uint1(0xFE) << Uint1(0xFF);
#endif
        break;
    }
    return str;
}


#include "ncbi_base64.c"


END_NCBI_SCOPE


// See in the header why it is outside of NCBI scope (SunPro bug workaround...)

#if defined(NCBI_USE_OLD_IOSTREAM)

extern NCBI_NS_NCBI::CNcbiOstream& operator<<(NCBI_NS_NCBI::CNcbiOstream& os,
                                              const NCBI_NS_STD::string& str)
{
    return str.empty() ? os : os << str.c_str();
}


extern NCBI_NS_NCBI::CNcbiIstream& operator>>(NCBI_NS_NCBI::CNcbiIstream& is,
                                              NCBI_NS_STD::string& str)
{
    int ch;
    if ( !is.ipfx() )
        return is;

    str.erase();

    SIZE_TYPE end = str.max_size();
    if ( is.width() )
        end = (streamsize) end < is.width() ? end : is.width(); 

    SIZE_TYPE i = 0;
    for (ch = is.rdbuf()->sbumpc();
         ch != EOF  &&  !isspace((unsigned char) ch);
         ch = is.rdbuf()->sbumpc()) {
        str.append(1, (char) ch);
        if (++i == end)
            break;
    }
    if (ch == EOF) 
        is.clear(NcbiEofbit | is.rdstate());      
    if ( !i )
        is.clear(NcbiFailbit | is.rdstate());      

    is.width(0);
    return is;
}

#endif  /* NCBI_USE_OLD_IOSTREAM */
