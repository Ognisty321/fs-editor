#include "automationhost.h"

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QCloseEvent>
#include <QColorDialog>
#include <QComboBox>
#include <QDialog>
#include <QDir>
#include <QDoubleSpinBox>
#include <QDoubleValidator>
#include <QFileDialog>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QSpinBox>
#include <QThread>
#include <QVBoxLayout>

#include <algorithm>
#include <climits>

// Aegisub's DialogProgress: the task, a bar, what the script writes (aegisub.debug.out, its errors)
// as it writes it, and Cancel. Cancel only asks the script to stop, so the window stays until it
// does; Esc and the window's close button press it. A run that wrote anything leaves the window
// open, Close in place of Cancel, until its text has been read.
class ScriptProgress : public QDialog {
public:
    explicit ScriptProgress(QWidget *parent) : QDialog(parent) {
        setWindowModality(Qt::ApplicationModal); // the script works on a copy: nothing may edit meanwhile
        setMinimumWidth(420);
        m_bar->setRange(0, 1000);
        m_bar->setValue(0);
        m_log->setReadOnly(true);
        m_log->hide();
        auto *buttons = new QHBoxLayout;
        buttons->addStretch();
        buttons->addWidget(m_button);
        auto *l = new QVBoxLayout(this);
        l->addWidget(m_label);
        l->addWidget(m_bar);
        l->addWidget(m_log, 1);
        l->addLayout(buttons);
        connect(m_button, &QPushButton::clicked, this, [this] { press(); });
    }

    std::function<void()> onCancel;

    void setValue(int v) { m_bar->setValue(v); }
    void setLabelText(const QString &t) { m_label->setText(t); }
    void setCancelButtonText(const QString &t) { m_button->setText(t); }
    void appendLog(const QString &t) {
        if (m_log->isHidden()) {
            m_log->show();
            resize(width(), std::max(height(), 360));
        }
        m_log->moveCursor(QTextCursor::End);
        m_log->insertPlainText(t);
        m_log->ensureCursorVisible();
    }
    // The script has ended: with text written, wait here for Close.
    void ended() {
        if (m_log->isHidden()) return;
        m_closing = true;
        m_bar->setValue(m_bar->maximum());
        m_button->setText(QObject::tr("Close"));
        exec();
    }

protected:
    void reject() override { press(); }
    void closeEvent(QCloseEvent *e) override {
        e->ignore();
        press();
    }

private:
    void press() {
        if (m_closing) accept();
        else if (onCancel) onCancel();
    }

    QLabel *m_label = new QLabel(this);
    QProgressBar *m_bar = new QProgressBar(this);
    QPlainTextEdit *m_log = new QPlainTextEdit(this);
    QPushButton *m_button = new QPushButton(QObject::tr("Cancel"), this);
    bool m_closing = false;
};

GuiAutomationHost::~GuiAutomationHost() { finish(); }

namespace {
bool onGuiThread() { return QThread::currentThread() == qApp->thread(); }

// f's result, computed on the GUI thread (the worker waits for it)
template <class F> auto onGui(F f) {
    if (onGuiThread()) return f();
    decltype(f()) r{};
    QMetaObject::invokeMethod(qApp, [&] { r = f(); }, Qt::BlockingQueuedConnection);
    return r;
}

// f on the GUI thread without waiting; directly (with the events handled) when already there
template <class F> void post(F f) {
    if (!onGuiThread()) return void(QMetaObject::invokeMethod(qApp, std::move(f), Qt::QueuedConnection));
    f();
    QApplication::processEvents();
}

// f's result with the progress window out of the way: a modal dialog must not sit under it
template <class F> auto aboveProgress(QPointer<ScriptProgress> &p, F f) {
    const bool shown = p && p->isVisible();
    if (shown) p->hide();
    auto r = f();
    if (shown && p) p->show();
    return r;
}

// Aegisub wildcards ("Text files (*.txt)|*.txt|All|*.*") to a Qt filter.
QString qtFilter(const QString &w) {
    const QStringList p = w.split('|');
    QStringList out;
    for (qsizetype i = 0; i + 1 < p.size(); i += 2) out << QString("%1 (%2)").arg(p[i], QString(p[i + 1]).replace(';', ' '));
    return out.isEmpty() ? w : out.join(";;");
}

// #RRGGBB[AA]; AA is Aegisub's alpha, 0 = opaque
QColor parseColor(const QString &s) {
    QColor c(s.left(7));
    if (s.size() >= 9) c.setAlpha(255 - s.mid(7, 2).toInt(nullptr, 16));
    return c;
}

QString colorText(const QColor &c, bool alpha) {
    QString s = QString::asprintf("#%02X%02X%02X", c.red(), c.green(), c.blue());
    if (alpha) s += QString::asprintf("%02X", 255 - c.alpha());
    return s;
}
} // namespace

fsb::DialogReply GuiAutomationHost::dialog(const fsb::DialogData &d) const {
    return onGui([&] {
        return aboveProgress(m_progress, [&] {
            QDialog dlg(m_parent);
            dlg.setWindowTitle(d.title.empty() ? QApplication::applicationDisplayName() : fs::q(d.title));
            auto *grid = new QGridLayout;
            std::vector<QWidget *> widgets;
            for (const auto &c : d.controls) {
                const QString cls = fs::q(c.class_name).toLower();
                QWidget *w = nullptr;
                if (cls == "label") {
                    w = new QLabel(fs::q(c.label), &dlg);
                } else if (cls == "edit" || cls == "alpha") {
                    w = new QLineEdit(fs::q(c.text), &dlg);
                } else if (cls == "textbox") {
                    auto *t = new QPlainTextEdit(fs::q(c.text), &dlg);
                    t->setMinimumHeight(std::max(1, c.height) * 24);
                    w = t;
                } else if (cls == "intedit") {
                    auto *s = new QSpinBox(&dlg);
                    s->setRange(c.has_min ? int(c.min) : INT_MIN, c.has_max ? int(c.max) : INT_MAX);
                    s->setValue(int(c.num));
                    w = s;
                } else if (cls == "floatedit" && c.step == 0) {
                    // Aegisub: no step is a plain text field, every digit the value has
                    auto *e = new QLineEdit(QString::number(c.num, 'g', 17), &dlg);
                    auto *v = new QDoubleValidator(e);
                    v->setLocale(QLocale::c());
                    if (c.has_min) v->setBottom(c.min);
                    if (c.has_max) v->setTop(c.max);
                    e->setValidator(v);
                    w = e;
                } else if (cls == "floatedit") {
                    auto *s = new QDoubleSpinBox(&dlg);
                    s->setDecimals(4);
                    s->setRange(c.has_min ? c.min : -1e9, c.has_max ? c.max : 1e9);
                    s->setSingleStep(c.step);
                    s->setValue(c.num);
                    w = s;
                } else if (cls == "dropdown") {
                    auto *b = new QComboBox(&dlg);
                    for (const auto &it : c.items) b->addItem(fs::q(it));
                    b->setCurrentText(fs::q(c.text));
                    w = b;
                } else if (cls == "checkbox") {
                    auto *b = new QCheckBox(fs::q(c.label), &dlg);
                    b->setChecked(c.flag);
                    w = b;
                } else if (cls == "color" || cls == "coloralpha") {
                    const bool alpha = cls == "coloralpha";
                    auto *b = new QPushButton(&dlg);
                    b->setProperty("color", fs::q(c.text));
                    auto paint = [b] {
                        QPixmap px(40, 14);
                        QColor col = parseColor(b->property("color").toString());
                        col.setAlpha(255);
                        px.fill(col);
                        b->setIcon(px);
                        b->setText(b->property("color").toString());
                    };
                    paint();
                    QObject::connect(b, &QPushButton::clicked, b, [b, paint, alpha] {
                        const QColor c = QColorDialog::getColor(parseColor(b->property("color").toString()), b, {},
                                                                alpha ? QColorDialog::ShowAlphaChannel
                                                                      : QColorDialog::ColorDialogOptions());
                        if (!c.isValid()) return;
                        b->setProperty("color", colorText(c, alpha));
                        paint();
                    });
                    w = b;
                } else {
                    w = new QLabel(fs::q(c.label), &dlg); // unknown class: show its label like Aegisub ignores it
                }
                if (!c.hint.empty()) w->setToolTip(fs::q(c.hint));
                grid->addWidget(w, c.y, c.x, std::max(1, c.height), std::max(1, c.width));
                widgets.push_back(w);
            }
            auto *v = new QVBoxLayout(&dlg);
            v->addLayout(grid);
            auto *buttons = new QHBoxLayout;
            buttons->addStretch();
            struct Button {
                QString shown, label, id; // label: what the script gets back
            };
            std::vector<Button> btns;
            for (size_t i = 0; i < d.buttons.size(); ++i) {
                const QString l = fs::q(d.buttons[i]);
                btns.push_back({l, l, i < d.button_ids.size() ? fs::q(d.button_ids[i]) : QString()});
            }
            // Aegisub's own OK and Cancel have no label: OK returns ""
            if (btns.empty()) btns = {{QObject::tr("OK"), "", "ok"}, {QObject::tr("Cancel"), "", "cancel"}};
            QString pressed;
            bool accepted = false, clicked = false;
            auto press = [&](const Button &b) {
                pressed = b.label;
                accepted = b.id != "cancel";
                clicked = true;
            };
            bool hasDefault = false;
            for (const auto &b : btns) {
                auto *pb = new QPushButton(b.shown, &dlg);
                // Aegisub: ok, yes or save is the default button
                if (!hasDefault && (b.id == "ok" || b.id == "yes" || b.id == "save")) {
                    pb->setDefault(true);
                    hasDefault = true;
                }
                QObject::connect(pb, &QPushButton::clicked, &dlg, [&, b] {
                    press(b);
                    dlg.done(accepted ? QDialog::Accepted : QDialog::Rejected);
                });
                buttons->addWidget(pb);
            }
            v->addLayout(buttons);
            dlg.exec();
            if (!clicked) { // Esc or the window's close button: the cancel, close or no button
                for (const char *id : {"cancel", "close", "no"}) {
                    auto it = std::find_if(btns.begin(), btns.end(), [id](const Button &b) { return b.id == id; });
                    if (it != btns.end()) {
                        press(*it);
                        break;
                    }
                }
            }

            fsb::DialogReply r;
            r.accepted = accepted;
            r.button = fs::rs(pressed);
            for (size_t i = 0; i < d.controls.size(); ++i) {
                fsb::DialogControlData c = d.controls[i];
                QWidget *w = widgets[i];
                if (auto *e = qobject_cast<QLineEdit *>(w)) {
                    if (fs::q(c.class_name).toLower() == "floatedit") c.num = e->text().toDouble();
                    else c.text = fs::rs(e->text());
                } else if (auto *t = qobject_cast<QPlainTextEdit *>(w)) c.text = fs::rs(t->toPlainText());
                else if (auto *sb = qobject_cast<QSpinBox *>(w)) c.num = sb->value();
                else if (auto *db = qobject_cast<QDoubleSpinBox *>(w)) c.num = db->value();
                else if (auto *cb = qobject_cast<QComboBox *>(w)) c.text = fs::rs(cb->currentText());
                else if (auto *ch = qobject_cast<QCheckBox *>(w)) c.flag = ch->isChecked();
                else if (auto *pb = qobject_cast<QPushButton *>(w)) c.text = fs::rs(pb->property("color").toString());
                r.controls.push_back(std::move(c));
            }
            return r;
        });
    });
}

rust::Vec<rust::String> GuiAutomationHost::open_files(rust::Str title, rust::Str dir, rust::Str file,
                                                       rust::Str wildcard, bool multiple, bool must_exist) const {
    const QString start = QDir(fs::q(dir)).filePath(fs::q(file)), caption = fs::q(title);
    const QString filter = qtFilter(fs::q(wildcard));
    const QStringList files = onGui([&] {
        return aboveProgress(m_progress, [&] {
            if (multiple) return QFileDialog::getOpenFileNames(m_parent, caption, start, filter);
            QFileDialog fd(m_parent, caption, start, filter);
            fd.setFileMode(must_exist ? QFileDialog::ExistingFile : QFileDialog::AnyFile);
            return fd.exec() ? fd.selectedFiles() : QStringList();
        });
    });
    return fs::rvec(files);
}

rust::String GuiAutomationHost::save_file(rust::Str title, rust::Str dir, rust::Str file, rust::Str wildcard,
                                          bool dont_prompt_overwrite) const {
    const QString start = QDir(fs::q(dir)).filePath(fs::q(file)), caption = fs::q(title);
    const QString filter = qtFilter(fs::q(wildcard));
    return fs::rs(onGui([&] {
        return aboveProgress(m_progress, [&] {
            return QFileDialog::getSaveFileName(m_parent, caption, start, filter, nullptr,
                                                dont_prompt_overwrite ? QFileDialog::DontConfirmOverwrite
                                                                      : QFileDialog::Options());
        });
    }));
}

ScriptProgress *GuiAutomationHost::bar() const {
    if (!m_progress) {
        m_progress = new ScriptProgress(m_parent);
        m_progress->onCancel = [this] {
            if (!m_cancelled.exchange(true)) {
                // a script that never asks is_cancelled runs on: a second press stops it
                m_progress->setCancelButtonText(QObject::tr("Stop script"));
            } else if (onStop) {
                onStop();
            }
        };
    }
    return m_progress;
}

template <class F> void GuiAutomationHost::queue(F set) const {
    bool first;
    {
        std::lock_guard lock(m_mx);
        set(m_pending);
        first = !std::exchange(m_pending.queued, true);
    }
    if (first) post([this] { flush(); });
}

void GuiAutomationHost::flush() const {
    Pending p;
    {
        std::lock_guard lock(m_mx);
        p = std::exchange(m_pending, {});
    }
    if (!m_running) {
        // after the run, or with none (a validate function's error): the log window has it
        if (!p.log.isEmpty() && onLog) onLog(p.log);
        return;
    }
    if (p.value < 0 && !p.task && !p.title && p.log.isEmpty()) return;
    ScriptProgress *b = bar();
    if (p.value >= 0) b->setValue(p.value);
    if (p.task) b->setLabelText(*p.task);
    if (p.title) b->setWindowTitle(*p.title);
    if (!p.log.isEmpty()) b->appendLog(p.log);
}

void GuiAutomationHost::progress(double percent) const {
    queue([v = int(std::clamp(percent, 0.0, 100.0) * 10)](Pending &p) { p.value = v; });
}

void GuiAutomationHost::task(rust::Str text) const {
    queue([t = fs::q(text)](Pending &p) { p.task = t; });
}

void GuiAutomationHost::title(rust::Str text) const {
    queue([t = fs::q(text)](Pending &p) { p.title = t; });
}

bool GuiAutomationHost::is_cancelled() const {
    if (onGuiThread()) QApplication::processEvents(); // a validate function runs here
    return m_cancelled;
}

void GuiAutomationHost::log(rust::Str text) const {
    queue([t = fs::q(text)](Pending &p) { p.log += t; });
}

void GuiAutomationHost::status_text(rust::Str text) const {
    post([this, t = fs::q(text)] {
        if (onStatus) onStatus(t);
    });
}

rust::String GuiAutomationHost::clipboard_get() const {
    return fs::rs(onGui([] { return QGuiApplication::clipboard()->text(); }));
}

bool GuiAutomationHost::clipboard_set(rust::Str text) const {
    return onGui([t = fs::q(text)] {
        QGuiApplication::clipboard()->setText(t);
        return true;
    });
}

void GuiAutomationHost::begin() const {
    m_cancelled = false;
    m_running = true;
}

void GuiAutomationHost::showProgress() const { bar()->show(); }

void GuiAutomationHost::finish() const {
    flush(); // what the script wrote last, into its window
    m_running = false;
    if (m_progress) {
        m_progress->ended();
        m_progress->hide();
        m_progress->deleteLater();
        m_progress = nullptr;
    }
}
