#include "fontcollector.h"

#include "tabpanel.h"
#include "videobox.h"

#include <QCheckBox>
#include <QDataStream>
#include <QDateTime>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontDatabase>
#include <QFormLayout>
#include <QFutureWatcher>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSettings>
#include <QtConcurrentRun>
#include <QTemporaryDir>
#include <QVBoxLayout>

#include <array>
#include <memory>
#include <utility>

namespace {
// The files in a .zip (Aegisub's and Kainote's font packing), deflated by Qt's own zlib; an error or "".
// shortcut: no ZIP64, so an archive stays under 4 GB; add it if anyone packs that much.
QString writeZip(const QString &path, const QStringList &files) {
    static const auto table = [] {
        std::array<quint32, 256> t{};
        for (quint32 i = 0; i < 256; ++i) {
            quint32 c = i;
            for (int k = 0; k < 8; ++k) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[i] = c;
        }
        return t;
    }();
    QFile out(path);
    if (!out.open(QIODevice::WriteOnly)) return out.errorString();
    QDataStream z(&out);
    z.setByteOrder(QDataStream::LittleEndian);
    QByteArray central;
    QDataStream c(&central, QIODevice::WriteOnly);
    c.setByteOrder(QDataStream::LittleEndian);
    const QDateTime now = QDateTime::currentDateTime();
    const auto time = quint16(now.time().hour() << 11 | now.time().minute() << 5 | now.time().second() / 2);
    const auto date = quint16((now.date().year() - 1980) << 9 | now.date().month() << 5 | now.date().day());
    for (const QString &file : files) {
        QFile in(file);
        if (!in.open(QIODevice::ReadOnly)) return file + ": " + in.errorString();
        const QByteArray data = in.readAll();
        quint32 crc = 0xFFFFFFFFu;
        for (const char ch : data) crc = table[(crc ^ quint8(ch)) & 0xFF] ^ (crc >> 8);
        crc ^= 0xFFFFFFFFu;
        // qCompress gives a 4-byte length, zlib's 2-byte header, the deflate data and a 4-byte checksum
        const QByteArray packed = qCompress(data, 9);
        const QByteArray raw = packed.mid(6, packed.size() - 10);
        const bool deflate = raw.size() < data.size();
        const QByteArray &body = deflate ? raw : data;
        const QByteArray name = QFileInfo(file).fileName().toUtf8();
        const auto offset = quint32(out.pos());
        auto header = [&](QDataStream &s, bool local) {
            s << quint32(local ? 0x04034b50 : 0x02014b50);
            if (!local) s << quint16(20); // made by
            s << quint16(20) << quint16(0x0800) << quint16(deflate ? 8 : 0) << time << date << crc
              << quint32(body.size()) << quint32(data.size()) << quint16(name.size()) << quint16(0);
            if (!local) s << quint16(0) << quint16(0) << quint16(0) << quint32(0) << offset;
            s.writeRawData(name.constData(), int(name.size()));
        };
        header(z, true);
        z.writeRawData(body.constData(), int(body.size()));
        header(c, false);
    }
    const auto at = quint32(out.pos());
    z.writeRawData(central.constData(), int(central.size()));
    z << quint32(0x06054b50) << quint16(0) << quint16(0) << quint16(files.size()) << quint16(files.size())
      << quint32(central.size()) << at << quint16(0);
    out.close();
    return out.error() == QFile::NoError ? QString() : out.errorString();
}
} // namespace

FontCollectorDialog::FontCollectorDialog(TabPanel *tab) : QDialog(tab) {
    setWindowTitle(tr("Fonts collector"));
    resize(760, 460);
    QSettings st;
    auto *f = new QFormLayout(this);
    auto folderRow = [this](QLineEdit *e) {
        auto *row = new QHBoxLayout;
        auto *b = new QPushButton(tr("Browse..."), this);
        connect(b, &QPushButton::clicked, this, [this, e] {
            const QString d = QFileDialog::getExistingDirectory(this, {}, e->text());
            if (!d.isEmpty()) e->setText(QDir::toNativeSeparators(d));
        });
        row->addWidget(e, 1);
        row->addWidget(b);
        return row;
    };
    auto *extra = new QLineEdit(st.value("fonts/extraDir").toString(), this);
    f->addRow(tr("Also search folder"), folderRow(extra));
    const QString media = tab->video()->hasVideo() ? tab->video()->path() : QString();
    auto *attached = new QCheckBox(tr("Fonts attached to the video"), this);
    attached->setEnabled(!media.isEmpty());
    attached->setChecked(!media.isEmpty());
    f->addRow(QString(), attached);
    auto *allTabs = new QCheckBox(tr("All open tabs"), this); // Kainote
    allTabs->setChecked(st.value("fonts/allTabs").toBool());
    f->addRow(QString(), allTabs);
    auto *dest = new QLineEdit(st.value("fonts/destDir").toString(), this);
    f->addRow(tr("Copy fonts to"), folderRow(dest));
    auto *report = new QPlainTextEdit(this);
    report->setReadOnly(true);
    report->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    f->addRow(report);
    auto *bb = new QDialogButtonBox(QDialogButtonBox::Close, this);
    auto *check = bb->addButton(tr("Check"), QDialogButtonBox::ActionRole);
    auto *copy = bb->addButton(tr("Copy"), QDialogButtonBox::ActionRole);
    // Aegisub's "Copy fonts to subtitle file's folder"
    const QString subsDir = tab->path().isEmpty() ? QString() : QFileInfo(tab->path()).absolutePath();
    auto *toSubs = bb->addButton(tr("Copy to the subtitles' folder"), QDialogButtonBox::ActionRole);
    toSubs->setEnabled(!subsDir.isEmpty());
    auto *attach = bb->addButton(tr("Attach to subtitles"), QDialogButtonBox::ActionRole);
    attach->setToolTip(tr("Embed the found fonts in the script's [Fonts] section (Aegisub)"));
    auto *pack = bb->addButton(tr("Pack into ZIP..."), QDialogButtonBox::ActionRole);
    // the script's fonts are read here, the font files on a worker (the first scan of the system's takes seconds)
    enum Mode { Check, Copy, ToSubs, Attach, Zip };
    auto run = [=, this](Mode mode, const QString &zip = {}) {
        if (mode == Copy && dest->text().isEmpty()) return report->setPlainText(tr("Choose a destination folder first."));
        QSettings s;
        s.setValue("fonts/extraDir", extra->text());
        s.setValue("fonts/destDir", dest->text());
        s.setValue("fonts/allTabs", allTabs->isChecked());
        auto uses = std::make_shared<rust::Box<fsb::FontUses>>(tab->s().font_uses());
        if (allTabs->isChecked())
            for (auto *t : tab->main()->tabs())
                if (t != tab) (*uses)->add(*t->s().font_uses());
        // attaching and packing collect the files into a folder of their own first, like a copy
        auto tmp = mode >= Attach ? std::make_shared<QTemporaryDir>() : nullptr;
        if (tmp && !tmp->isValid()) return report->setPlainText(tmp->errorString());
        const QString dir = extra->text(), from = attached->isChecked() ? media : QString(),
                      to = mode == Copy ? dest->text() : mode == ToSubs ? subsDir : tmp ? tmp->path() : QString();
        for (auto *b : {check, copy, toSubs, attach, pack}) b->setEnabled(false);
        report->setPlainText(tr("Searching for fonts..."));
        using Result = std::pair<bool, QString>; // ok, report or error
        auto *watch = new QFutureWatcher<Result>(this);
        connect(watch, &QFutureWatcher<Result>::finished, this, [=, this] {
            auto [ok, r] = watch->result();
            const int missing = r.count("MISSING") + r.count("missing glyphs");
            QString done = mode == Copy || mode == ToSubs ? tr("Copied to %1").arg(QDir::toNativeSeparators(to)) : QString();
            QStringList files;
            if (ok && tmp)
                for (const QFileInfo &fi : QDir(tmp->path()).entryInfoList(QDir::Files)) files << fi.filePath();
            if (ok && mode == Zip) {
                const QString err = writeZip(zip, files);
                if (!err.isEmpty()) ok = false, r = err;
                done = tr("Packed into %1").arg(QDir::toNativeSeparators(zip));
            }
            if (ok && mode == Attach) {
                const auto paths = fs::rvec(files);
                try {
                    const size_t n = tab->s().attach_fonts({paths.data(), paths.size()});
                    if (n) tab->changed();
                    done = tr("Attached %1 font file(s) to the subtitles").arg(n);
                } catch (const rust::Error &e) {
                    ok = false;
                    r = QString::fromUtf8(e.what());
                }
            }
            report->setPlainText(!ok ? r
                                     : r + "\n" + (missing ? tr("%n problem(s) found.", nullptr, missing) : tr("All fonts found.")) +
                                           (done.isEmpty() ? QString() : "\n" + done));
            for (auto *b : {check, copy, attach, pack}) b->setEnabled(true);
            toSubs->setEnabled(!subsDir.isEmpty());
            watch->deleteLater();
        });
        watch->setFuture(QtConcurrent::run([uses, dir, from, to]() -> Result {
            rust::Vec<rust::String> dirs;
            if (!dir.isEmpty()) dirs.push_back(fs::rs(dir));
            // Kainote's external fonts folder is searched like the system's
            if (const QString ext = QSettings().value("fonts/externalDir").toString(); !ext.isEmpty())
                dirs.push_back(fs::rs(ext));
            try {
                return {true, fs::q(fsb::collect_fonts(**uses, std::move(dirs), fs::Utf8(from), fs::Utf8(to)))};
            } catch (const std::exception &e) {
                return {false, QString::fromUtf8(e.what())};
            }
        }));
    };
    connect(check, &QPushButton::clicked, this, [run] { run(Check); });
    connect(copy, &QPushButton::clicked, this, [run] { run(Copy); });
    connect(toSubs, &QPushButton::clicked, this, [run] { run(ToSubs); });
    connect(attach, &QPushButton::clicked, this, [run] { run(Attach); });
    connect(pack, &QPushButton::clicked, this, [=, this] {
        const QString start = dest->text().isEmpty() ? QFileInfo(tab->path()).absolutePath() : dest->text();
        const QString zip = QFileDialog::getSaveFileName(this, tr("Pack fonts into a ZIP archive"),
                                                         QDir(start).filePath("fonts.zip"), tr("ZIP archives (*.zip)"));
        if (!zip.isEmpty()) run(Zip, zip);
    });
    connect(bb, &QDialogButtonBox::rejected, this, &QDialog::reject);
    f->addRow(bb);
}
