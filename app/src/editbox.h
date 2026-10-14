// The edit box (Kainote layout): tag bar, info row, TL original editor,
// text editor with tag highlighting and the fields row. Edits are applied
// live; consecutive edits of one field merge into one undo step.
#pragma once

#include "bridge.h"

#include <QLineEdit>
#include <QPlainTextEdit>
#include <QWidget>

class TabPanel;
class QCheckBox;
class QComboBox;
class QLabel;
class QSpinBox;
class QRadioButton;
class QToolButton;
class QHBoxLayout;
class QCompleter;

// Start/End/Duration field: h:mm:ss.cc in overwrite mode, or a frame number.
// Hunspell spell checking shared by every text box (settings spell/enabled, spell/dir, spell/lang).
namespace spell {
fsb::Speller *get(); // nullptr when disabled or no dictionary could be loaded
void reload();       // after a settings change: reload the dictionary, re-highlight
QString dir();
QStringList languages();
} // namespace spell

class TimeEdit : public QLineEdit {
    Q_OBJECT
public:
    explicit TimeEdit(QWidget *parent = nullptr);
    void setFrames(bool on);
    void setMs(qint64 ms, qint64 frame);
    qint64 ms() const;      // -1 when invalid
    qint64 frame() const;
signals:
    void edited();
protected:
    void wheelEvent(QWheelEvent *) override;
private:
    bool m_frames = false;
};

class SubsTextEdit : public QPlainTextEdit {
    Q_OBJECT
public:
    explicit SubsTextEdit(QWidget *parent = nullptr);
    QString text() const { return toPlainText(); }
    void setText(const QString &t);
    void insertTag(const QString &tag);
    void toggleTag(const QString &name);
signals:
    void keyCommand(int key, Qt::KeyboardModifiers mods);
protected:
    void keyPressEvent(QKeyEvent *) override;
    void contextMenuEvent(QContextMenuEvent *) override;
private:
    QCompleter *m_completer;
};

class EditBox : public QWidget {
    Q_OBJECT
public:
    explicit EditBox(TabPanel *tab);
    void load(int row);
    void reload();
    void focusText();
    int row() const { return m_row; }

protected:
    void resizeEvent(QResizeEvent *) override;

private:
    void commit(const char *field);
    void updateInfo(const fsb::Row &r);
    void rebuildTagButtons();
    void onKey(int key, Qt::KeyboardModifiers mods);
    void nextLine();
    void setColorTag(int which);
    void configureTagButton(int index);
    void wrapFields();

    TabPanel *m_tab;
    int m_row = -1;
    bool m_loading = false;
    bool m_framesMode = false;

    QHBoxLayout *m_tagBar, *m_userTags;
    QComboBox *m_an;
    QLabel *m_lineLabel, *m_wrapsLabel, *m_cpsLabel;
    QCheckBox *m_tlMode;
    QRadioButton *m_timeRadio, *m_frameRadio;
    QWidget *m_tlBox;
    SubsTextEdit *m_original, *m_text;
    QCheckBox *m_hideOriginal, *m_doubtful, *m_movingTags;
    QCheckBox *m_comment;
    QSpinBox *m_layer, *m_ml, *m_mr, *m_mv;
    TimeEdit *m_start, *m_end, *m_duration;
    QComboBox *m_style, *m_actor, *m_effect;
    QToolButton *m_styleEdit;
    QWidget *m_fields1, *m_fields2;
    QHBoxLayout *m_row1, *m_row2;
    QList<QWidget *> m_wrapWidgets;
};
