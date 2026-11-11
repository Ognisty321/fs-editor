// The fields picked for "Paste lines over" and Kainote's copy of columns.
#pragma once

class QWidget;

// Aegisub's "Select fields to paste over": the Session::paste_over bits, or -1 when cancelled.
// Fields of a line as bits 1 comment .. 1024 text; `copy` (Kainote's copy columns) adds 2048 text without tags.
int chooseFields(QWidget *parent, bool copy = false);
