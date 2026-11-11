#include "attachmentsdialog.h"

#include "tabpanel.h"

#include <QFileDialog>
#include <QFileInfo>
#include <QHeaderView>
#include <QLocale>
#include <QMessageBox>
#include <QPushButton>
#include <QTreeWidget>
#include <QVBoxLayout>

AttachmentsDialog::AttachmentsDialog(QWidget *parent, TabPanel *tab) : QDialog(parent), m_tab(tab) {
    setWindowTitle(tr("Attachment list"));
    resize(520, 340);
    auto *v = new QVBoxLayout(this);
    m_list = new QTreeWidget(this);
    m_list->setHeaderLabels({tr("Attachment name"), tr("Size"), tr("Group")});
    m_list->setRootIsDecorated(false);
    m_list->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_list->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    v->addWidget(m_list);
    auto *h = new QHBoxLayout;
    auto button = [&](const QString &text, auto fn) {
        auto *b = new QPushButton(text, this);
        connect(b, &QPushButton::clicked, this, fn);
        h->addWidget(b);
        return b;
    };
    auto attach = [this](bool font) {
        const QStringList files = QFileDialog::getOpenFileNames(
            this, font ? tr("Attach font") : tr("Attach graphics"), QFileInfo(m_tab->path()).absolutePath(),
            font ? tr("Fonts (*.ttf *.otf *.ttc *.pfb *.pfm)") : tr("Graphics (*.bmp *.gif *.jpg *.jpeg *.ico *.wmf *.png)"));
        for (const QString &f : files) {
            try {
                m_tab->s().attach_file(font, fs::Utf8(f));
            } catch (const std::exception &e) {
                QMessageBox::warning(this, windowTitle(), QString::fromUtf8(e.what()));
            }
        }
        m_tab->changed();
        refresh();
    };
    button(tr("Attach font"), [attach] { attach(true); });
    button(tr("Attach graphics"), [attach] { attach(false); });
    auto *extract = button(tr("Extract"), [this] {
        const auto items = m_list->selectedItems();
        if (items.isEmpty()) return;
        auto save = [this](QTreeWidgetItem *it, const QString &path) {
            try {
                m_tab->s().extract_attachment(it->data(0, Qt::UserRole).toBool(), fs::Utf8(it->text(0)), fs::Utf8(path));
            } catch (const std::exception &e) {
                QMessageBox::warning(this, windowTitle(), QString::fromUtf8(e.what()));
            }
        };
        const QString base = QFileInfo(m_tab->path()).absolutePath();
        if (items.size() == 1) {
            const QString f = QFileDialog::getSaveFileName(
                this, tr("Extract"), base + "/" + fs::q(fsb::attachment_file_name(fs::Utf8(items[0]->text(0)))));
            if (!f.isEmpty()) save(items[0], f);
        } else {
            const QString dir = QFileDialog::getExistingDirectory(this, tr("Extract to folder"), base);
            if (dir.isEmpty()) return;
            for (auto *it : items) save(it, dir + "/" + fs::q(fsb::attachment_file_name(fs::Utf8(it->text(0)))));
        }
    });
    auto *remove = button(tr("Delete"), [this] {
        for (auto *it : m_list->selectedItems())
            m_tab->s().remove_attachment(it->data(0, Qt::UserRole).toBool(), fs::Utf8(it->text(0)));
        m_tab->changed();
        refresh();
    });
    h->addStretch();
    button(tr("Close"), [this] { accept(); });
    v->addLayout(h);
    auto sync = [this, extract, remove] {
        const bool any = !m_list->selectedItems().isEmpty();
        extract->setEnabled(any);
        remove->setEnabled(any);
    };
    connect(m_list, &QTreeWidget::itemSelectionChanged, this, sync);
    refresh();
    sync();
}

void AttachmentsDialog::refresh() {
    m_list->clear();
    for (const auto &a : m_tab->s().attachments()) {
        auto *it = new QTreeWidgetItem(m_list, {fs::q(a.name), QLocale().formattedDataSize(qint64(a.size)),
                                                a.font ? tr("Fonts") : tr("Graphics")});
        it->setData(0, Qt::UserRole, a.font);
    }
}
