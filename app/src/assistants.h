// Aegisub's translation and styling assistants.
#pragma once

#include <QDialog>
#include <QPointer>
#include <span>
#include <vector>

class TabPanel;
class MainWindow;
class QLineEdit;
class QCheckBox;
class QListWidget;
class QLabel;
class QPlainTextEdit;

// Aegisub's translation assistant: the text blocks of the lines one after another from the active
// line, the original above and the translation typed below put in its place.
class TranslationAssistant : public QDialog {
    Q_OBJECT
public:
    TranslationAssistant(MainWindow *main, TabPanel *tab);
    static std::span<const struct PanelKey> keys(); // its own keys (Aegisub's Translation Assistant)
    void runCommand(const QString &id);
    bool start(); // false (and closed) when there is nothing to translate from the active line on

protected:
    bool eventFilter(QObject *o, QEvent *e) override;

private:
    std::vector<uint32_t> blocks() const; // the line's, as translatable_blocks
    bool step(int dir); // to the next (1) or previous (-1) block, across lines
    void lineChanged(); // the active line, picked elsewhere, from its first block
    void display();
    void commit(bool next);
    MainWindow *m_main;
    QPointer<TabPanel> m_tab;
    class QLabel *m_lineNo;
    QPlainTextEdit *m_original;
    class SubsTextEdit *m_translation;
    QCheckBox *m_preview;
    int m_row = 0, m_block = 0;
    bool m_busy = false; // the assistant's own edits and moves
};

// Aegisub's styling assistant: type the start of a style name, Enter sets it on
// the active line and moves to the next one.
class StylingAssistant : public QDialog {
    Q_OBJECT
public:
    StylingAssistant(MainWindow *main, TabPanel *tab);
    static std::span<const struct PanelKey> keys(); // its own keys (Aegisub's Styling Assistant)
    void runCommand(const QString &id);

protected:
    bool eventFilter(QObject *o, QEvent *e) override;

private:
    void lineChanged();
    void commit(bool next);
    MainWindow *m_main;
    QPointer<TabPanel> m_tab;
    QPlainTextEdit *m_text;
    QListWidget *m_styles;
    QLineEdit *m_name;
    QCheckBox *m_seek;
};
