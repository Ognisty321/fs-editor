// Rust core access for the GUI: the cxx-generated API plus Qt conversions.
#pragma once

#include "fs-bridge/lib.rs.h"

#include <QString>
#include <QStringList>
#include <algorithm>
#include <functional>
#include <vector>

namespace fs {

inline QString q(const rust::String &s) { return QString::fromUtf8(s.data(), qsizetype(s.size())); }
inline QString q(rust::Str s) { return QString::fromUtf8(s.data(), qsizetype(s.size())); }
// a name from data (a file, style, track, macro) as a menu, button or tab text: its & no mnemonic
inline QString menuText(QString s) { return s.replace('&', QStringLiteral("&&")); }
// a menu text as read: && one &, a lone & only marks the mnemonic
inline QString plainText(QString s) {
    s.replace(QStringLiteral("&&"), QChar(0));
    s.remove('&');
    return s.replace(QChar(0), '&');
}

// Keeps the UTF-8 bytes alive for the duration of a call taking rust::Str.
struct Utf8 {
    QByteArray b;
    explicit Utf8(const QString &s) : b(s.toUtf8()) {}
    operator rust::Str() const { return rust::Str(b.constData(), size_t(b.size())); }
};

inline rust::String rs(const QString &s) {
    QByteArray b = s.toUtf8();
    return rust::String(b.constData(), size_t(b.size()));
}

inline QStringList qlist(const rust::Vec<rust::String> &v) {
    QStringList out;
    for (const auto &s : v) out << q(s);
    return out;
}

inline rust::Vec<rust::String> rvec(const QStringList &l) {
    rust::Vec<rust::String> v;
    for (const auto &s : l) v.push_back(rs(s));
    return v;
}

inline rust::Slice<const size_t> slice(const std::vector<size_t> &v) {
    return {v.data(), v.size()};
}

inline std::vector<size_t> vec(const rust::Vec<size_t> &v) { return {v.begin(), v.end()}; }

// fsb::Progress handing whole percents to `fn` on the opening thread; `fn` returns false to cancel.
class PercentProgress final : public fsb::Progress {
public:
    explicit PercentProgress(std::function<bool(int)> fn) : m_fn(std::move(fn)) {}
    bool report(int64_t done, int64_t total) const override {
        const int pct = total > 0 ? int(std::clamp<int64_t>(done * 100 / total, 0, 100)) : 0;
        if (pct != m_last) {
            m_last = pct;
            m_go = m_fn(pct);
        }
        return m_go;
    }

private:
    std::function<bool(int)> m_fn;
    mutable int m_last = -1;
    mutable bool m_go = true;
};

inline QString fmtTime(qint64 ms) { return q(fsb::fmt_time(ms)); }
inline qint64 parseTime(const QString &s) { return fsb::parse_time(Utf8(s)); }

} // namespace fs
