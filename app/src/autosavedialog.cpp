#include "autosavedialog.h"

#include "dialogutil.h"

#include <QCheckBox>
#include <QDateTime>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileInfo>
#include <QGroupBox>
#include <QLineEdit>
#include <QListWidget>
#include <QLocale>
#include <QPushButton>
#include <QVBoxLayout>

#include <algorithm>
#include <functional>
#include <numeric>
#include <utility>
#include <vector>

AutosaveDialog::AutosaveDialog(QWidget *parent) : QDialog(parent) {
    setWindowTitle(tr("Open autosave file"));
    resize(800, 350);
    auto *v = new QVBoxLayout(this);
    auto *h = new QHBoxLayout;
    auto *files = new QGroupBox(tr("Files"), this);
    auto *versions = new QGroupBox(tr("Versions"), this);
    m_files = new QListWidget(files);
    m_versions = new QListWidget(versions);
    (new QVBoxLayout(files))->addWidget(m_files);
    (new QVBoxLayout(versions))->addWidget(m_versions);
    h->addWidget(files);
    h->addWidget(versions);
    // Kainote's Find files: the names with all (or any) of the words typed
    auto *find = new QGroupBox(tr("Find files"), this);
    auto *fh = new QHBoxLayout(find);
    auto *words = new QLineEdit(find);
    auto *allWords = new QCheckBox(tr("All words"), find);
    allWords->setChecked(true);
    fh->addWidget(words, 1);
    fh->addWidget(allWords);
    v->addWidget(find);
    const auto filter = [this, words, allWords] {
        const QStringList ws = words->text().split(' ', Qt::SkipEmptyParts);
        int first = -1;
        for (int i = 0; i < m_files->count(); ++i) {
            const QString name = m_files->item(i)->text();
            const auto has = [&name](const QString &w) { return name.contains(w, Qt::CaseInsensitive); };
            const bool shown = ws.isEmpty() || (allWords->isChecked() ? std::ranges::all_of(ws, has) : std::ranges::any_of(ws, has));
            m_files->item(i)->setHidden(!shown);
            if (shown && first < 0) first = i;
        }
        if (!m_files->currentItem() || m_files->currentItem()->isHidden()) m_files->setCurrentRow(first);
    };
    connect(words, &QLineEdit::textChanged, this, filter);
    connect(allWords, &QCheckBox::toggled, this, filter);
    v->addLayout(h, 1);
    auto *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    bb->button(QDialogButtonBox::Ok)->setText(tr("Open"));
    v->addWidget(bb);
    connect(bb, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(m_versions, &QListWidget::itemDoubleClicked, this, &QDialog::accept);

    const QString suffix = ".AUTOSAVE.ass";
    QMap<QString, QList<std::pair<QDateTime, QString>>> byName;
    for (const QFileInfo &fi : QDir(autosaveDir()).entryInfoList({"*" + suffix, "*.ORIGINAL.*"}, QDir::Files)) {
        if (!fi.fileName().endsWith(suffix)) { // the file as it was opened (Aegisub)
            byName[fi.fileName().section(".ORIGINAL.", 0, 0)].append({fi.lastModified(), fi.absoluteFilePath()});
            continue;
        }
        const QString stem = fi.fileName().chopped(suffix.size());
        const QDateTime date = QDateTime::fromString(stem.section('.', -1), "yyyy-MM-dd-HH-mm-ss");
        const QString name = date.isValid() ? stem.section('.', 0, -2) : stem;
        byName[name].append({date.isValid() ? date : fi.lastModified(), fi.absoluteFilePath()});
    }
    QList<QList<std::pair<QDateTime, QString>>> groups = byName.values();
    QStringList names = byName.keys();
    for (auto &g : groups) std::ranges::sort(g, std::greater{});
    std::vector<int> order(groups.size());
    std::iota(order.begin(), order.end(), 0);
    std::ranges::sort(order, [&](int a, int b) { return groups[a][0].first > groups[b][0].first; });
    for (int i : order) {
        auto *it = new QListWidgetItem(names[i], m_files);
        QStringList paths, labels;
        for (const auto &[d, p] : groups[i]) {
            paths << p;
            labels << QLocale().toString(d, QLocale::LongFormat) +
                          (p.endsWith(suffix) ? QString() : "  " + tr("(the file as opened)"));
        }
        it->setData(Qt::UserRole, paths);
        it->setData(Qt::UserRole + 1, labels);
    }
    connect(m_files, &QListWidget::currentRowChanged, this, [this](int row) {
        m_versions->clear();
        if (row < 0) return;
        const QStringList paths = m_files->item(row)->data(Qt::UserRole).toStringList();
        const QStringList labels = m_files->item(row)->data(Qt::UserRole + 1).toStringList();
        for (int k = 0; k < paths.size(); ++k) (new QListWidgetItem(labels[k], m_versions))->setData(Qt::UserRole, paths[k]);
        m_versions->setCurrentRow(0);
    });
    bb->button(QDialogButtonBox::Ok)->setEnabled(m_files->count() > 0);
    m_files->setCurrentRow(0);
}

QString AutosaveDialog::chosen() const {
    auto *it = m_versions->currentItem();
    return it ? it->data(Qt::UserRole).toString() : QString();
}
