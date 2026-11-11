#include "updates.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDesktopServices>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPushButton>
#include <QRegularExpression>
#include <QSettings>
#include <QUrl>

namespace {
// The newest release that is no draft and no pre-release, as GitHub tells it.
const char *const kLatest = "https://api.github.com/repos/Ognisty321/fs-editor/releases/latest";
const char *const kReleases = "https://github.com/Ognisty321/fs-editor/releases";
const qint64 kDay = 24 * 60 * 60;

QList<int> numbers(const QString &version) {
    QList<int> n;
    static const QRegularExpression digits("\\d+");
    for (auto it = digits.globalMatch(version); it.hasNext();) n << it.next().captured().toInt();
    return n;
}

void answer(QWidget *parent, bool manual, const QByteArray &body, const QString &error) {
    const QString title = QCoreApplication::translate("updates", "Check for updates");
    const QJsonObject release = QJsonDocument::fromJson(body).object();
    const QString latest = release.value("tag_name").toString();
    if (!error.isEmpty() || numbers(latest).isEmpty()) {
        if (manual)
            QMessageBox::warning(parent, title,
                                 QCoreApplication::translate("updates", "The newest version could not be read.\n%1")
                                     .arg(error.isEmpty() ? QCoreApplication::translate("updates", "The answer names no version.")
                                                          : error));
        return;
    }
    QSettings st;
    const QString current = QCoreApplication::applicationVersion();
    if (updates::compare(latest, current) <= 0) {
        if (manual)
            QMessageBox::information(
                parent, title, QCoreApplication::translate("updates", "You have the newest version (%1).").arg(current));
        return;
    }
    if (!manual && updates::compare(latest, st.value("main/updateSkip").toString()) <= 0) return;
    QMessageBox box(QMessageBox::Information, title,
                    QCoreApplication::translate("updates", "FS Editor %1 is available. You have %2.")
                        .arg(latest.startsWith('v') ? latest.mid(1) : latest, current),
                    QMessageBox::NoButton, parent);
    // the release's own notes, as its page shows them
    if (const QString notes = release.value("body").toString().trimmed(); !notes.isEmpty()) box.setDetailedText(notes);
    auto *open = box.addButton(QCoreApplication::translate("updates", "Open the download page"), QMessageBox::AcceptRole);
    auto *skip = box.addButton(QCoreApplication::translate("updates", "Skip this version"), QMessageBox::DestructiveRole);
    box.addButton(QCoreApplication::translate("updates", "Remind me later"), QMessageBox::RejectRole);
    box.exec();
    if (box.clickedButton() == open) {
        // only a page of the project's releases is opened, whatever the answer held
        const QUrl page(release.value("html_url").toString());
        QDesktopServices::openUrl(page.toString().startsWith(kReleases) ? page : QUrl(kReleases));
    } else if (box.clickedButton() == skip) {
        st.setValue("main/updateSkip", latest);
    }
}
} // namespace

int updates::compare(const QString &a, const QString &b) {
    const QList<int> x = numbers(a), y = numbers(b);
    for (qsizetype i = 0; i < std::max(x.size(), y.size()); ++i)
        if (const int d = x.value(i) - y.value(i)) return d < 0 ? -1 : 1;
    return 0;
}

void updates::check(QWidget *parent, bool manual) {
    static QNetworkAccessManager *net = new QNetworkAccessManager(qApp);
    // FSED_UPDATE_URL: another place to ask, e.g. a file with an answer, for a check of this window
    const QString other = qEnvironmentVariable("FSED_UPDATE_URL");
    if (other.isEmpty() && qEnvironmentVariableIsSet("FSED_SMOKE")) return; // a smoke test asks no one
    QNetworkRequest req(other.isEmpty() ? QUrl(kLatest) : QUrl::fromUserInput(other));
    req.setRawHeader("Accept", "application/vnd.github+json");
    req.setHeader(QNetworkRequest::UserAgentHeader, "fs-editor");
    req.setTransferTimeout(15000);
    QNetworkReply *reply = net->get(req);
    QObject::connect(reply, &QNetworkReply::finished, parent, [reply, parent, manual] {
        reply->deleteLater();
        const bool failed = reply->error() != QNetworkReply::NoError;
        answer(parent, manual, failed ? QByteArray() : reply->readAll(), failed ? reply->errorString() : QString());
    });
}

void updates::checkAtStart(QWidget *parent) {
    QSettings st;
    if (!st.value("main/checkUpdates", false).toBool()) return;
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    if (now < st.value("main/updateNextCheck", 0).toLongLong()) return;
    st.setValue("main/updateNextCheck", now + kDay);
    check(parent, false);
}
