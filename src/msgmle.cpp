﻿/*
 * msgmle.cpp - subclass of PsiTextView to handle various hotkeys
 * Copyright (C) 2001-2003  Justin Karneges, Michail Pishchagin
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 *
 */

#include "msgmle.h"

#include "htmltextcontroller.h"
#include "psiiconset.h"
#include "psioptions.h"
#include "qiteaudiorecorder.h"
#include "shortcutmanager.h"
#include "spellchecker/spellchecker.h"
#include "spellchecker/spellhighlighter.h"
#include "textutil.h"

#include <QAbstractTextDocumentLayout>
#include <QAction>
#include <QApplication>
#include <QAudioRecorder>
#include <QClipboard>
#include <QDesktopWidget>
#include <QEvent>
#include <QKeyEvent>
#include <QLabel>
#include <QLayout>
#include <QMenu>
#include <QMimeData>
#include <QMimeDatabase>
#include <QResizeEvent>
#include <QStyle>
#include <QTextCharFormat>
#include <QTextDocument>
#include <QTimer>
#include <QToolButton>

static const int           TIMEOUT        = 30000; // 30 secs maximum time interval
static const int           SECOND         = 1000;
static const int           maxOverlayTime = TIMEOUT / SECOND;
static const QLatin1String capOption("options.ui.chat.auto-capitalize");
static const QLatin1String audioMessage("options.media.audio-message");
static const QLatin1String spellOption("options.ui.spell-check.enabled");

//----------------------------------------------------------------------------
// CapitalLettersController
//----------------------------------------------------------------------------

class CapitalLettersController : public QObject {
    Q_OBJECT
public:
    explicit CapitalLettersController(QTextEdit *parent) : QObject(), te_(parent), enabled_(true)
    {
        connect(te_->document(), &QTextDocument::contentsChange, this, &CapitalLettersController::textChanged);
    }

    ~CapitalLettersController() override = default;

    void setEnabled(bool enabled) { enabled_ = enabled; }
    bool isEnabled() const { return enabled_; }

private:
    void capitalizeChar(int pos, QChar c) { changeChar(pos, c.toUpper()); }

    void decapitalizeChar(int pos, QChar c) { changeChar(pos, c.toLower()); }

    void changeChar(int pos, QChar c)
    {
        QTextCursor cur = te_->textCursor();
        cur.setPosition(pos + 1);
        const QTextCharFormat cf = cur.charFormat();
        cur.deletePreviousChar();
        cur.setCharFormat(cf);
        cur.insertText(c);
    }

public slots:
    void textChanged(int pos, int /*charsRemoved*/, int charsAdded)
    {
        if (enabled_) {
            if (charsAdded == 0) {
                return;
            }
            if (!te_->textCursor().atEnd()) { // Editing the letter in the middle of the text
                return;
            }
            bool capitalizeNext_ = false;

            if (pos == 0 && charsAdded < 3) { // the first letter after the previous message was sent
                capitalizeNext_ = true;
            } else if (charsAdded > 1) { // Insert a piece of text
                return;
            } else {
                QRegExp capitalizeAfter("(?:^[^.][.]+\\s+)|(?:\\s*[^.]{2,}[.]+\\s+)|(?:[!?]\\s+)");
                int     index = te_->toPlainText().lastIndexOf(capitalizeAfter);
                if (index != -1 && index == pos - capitalizeAfter.matchedLength()) {
                    capitalizeNext_ = true;
                }
            }

            if (capitalizeNext_) {
                QChar ch = te_->document()->characterAt(pos);
                if (!ch.isLetter() || !ch.isLower()) {
                    return;
                } else {
                    capitalizeChar(pos, ch);
                }
            }
        }
    }

    void changeCase()
    {
        bool tmpEnabled    = enabled_;
        enabled_           = false;
        QTextCursor oldCur = te_->textCursor();
        int         pos    = oldCur.position();
        int         begin  = 0;
        int         end    = te_->document()->characterCount();
        if (oldCur.hasSelection()) {
            begin = oldCur.selectionStart();
            end   = oldCur.selectionEnd();
        }
        for (; begin < end; begin++) {
            QChar ch = te_->document()->characterAt(begin);
            if (!ch.isLetter()) {
                continue;
            }

            if (ch.isLower()) {
                capitalizeChar(begin, ch);
            } else {
                decapitalizeChar(begin, ch);
            }
        }
        oldCur.setPosition(pos);
        te_->setTextCursor(oldCur);
        enabled_ = tmpEnabled;
    }

private:
    QTextEdit *te_;
    bool       enabled_;
};

//----------------------------------------------------------------------------
// ChatEdit
//----------------------------------------------------------------------------
ChatEdit::ChatEdit(QWidget *parent) :
    QTextEdit(parent), layout_(nullptr), recButton_(nullptr), overlay_(nullptr), timeout_(TIMEOUT)
{
    controller_  = new HTMLTextController(this);
    capitalizer_ = new CapitalLettersController(this);

    setWordWrapMode(QTextOption::WordWrap);
    setAcceptRichText(false);

    setReadOnly(false);
    setUndoRedoEnabled(true);

    setMinimumHeight(48);

    previous_position_ = 0;
    setCheckSpelling(checkSpellingGloballyEnabled());
    connect(PsiOptions::instance(), &PsiOptions::optionChanged, this, &ChatEdit::optionsChanged);
    typedMsgsIndex = 0;
    initActions();
    setShortcuts();
    optionsChanged(spellOption);
    optionsChanged(capOption);
    optionsChanged(audioMessage);
}

ChatEdit::~ChatEdit()
{
    clearMessageHistory();
    delete controller_;
    delete capitalizer_;
}

CapitalLettersController *ChatEdit::capitalizer() { return capitalizer_; }

void ChatEdit::initActions()
{
    act_showMessagePrev = new QAction(this);
    addAction(act_showMessagePrev);
    connect(act_showMessagePrev, &QAction::triggered, this, &ChatEdit::showHistoryMessagePrev);

    act_showMessageNext = new QAction(this);
    addAction(act_showMessageNext);
    connect(act_showMessageNext, &QAction::triggered, this, &ChatEdit::showHistoryMessageNext);

    act_showMessageFirst = new QAction(this);
    addAction(act_showMessageFirst);
    connect(act_showMessageFirst, &QAction::triggered, this, &ChatEdit::showHistoryMessageFirst);

    act_showMessageLast = new QAction(this);
    addAction(act_showMessageLast);
    connect(act_showMessageLast, &QAction::triggered, this, &ChatEdit::showHistoryMessageLast);

    act_changeCase = new QAction(this);
    addAction(act_changeCase);
    connect(act_changeCase, &QAction::triggered, capitalizer_, &CapitalLettersController::changeCase);

    QClipboard *clipboard = QApplication::clipboard();
    actPasteAsQuote_      = new QAction(tr("Paste as Quotation"), this);
    actPasteAsQuote_->setEnabled(clipboard->mimeData()->hasText());
    addAction(actPasteAsQuote_);
    connect(actPasteAsQuote_, &QAction::triggered, this, &ChatEdit::pasteAsQuote);
    connect(clipboard, &QClipboard::dataChanged, this, &ChatEdit::changeActPasteAsQuoteState);
}

void ChatEdit::setShortcuts()
{
    act_showMessagePrev->setShortcuts(ShortcutManager::instance()->shortcuts("chat.show-messagePrev"));
    act_showMessageNext->setShortcuts(ShortcutManager::instance()->shortcuts("chat.show-messageNext"));
    act_showMessageFirst->setShortcuts(ShortcutManager::instance()->shortcuts("chat.show-messageFirst"));
    act_showMessageLast->setShortcuts(ShortcutManager::instance()->shortcuts("chat.show-messageLast"));
    act_changeCase->setShortcuts(ShortcutManager::instance()->shortcuts("chat.change-case"));
}

void ChatEdit::setDialog(QWidget *dialog) { dialog_ = dialog; }

QSize ChatEdit::sizeHint() const { return minimumSizeHint(); }

void ChatEdit::setFont(const QFont &f)
{
    QTextEdit::setFont(f);
    controller_->setFont(f);
}

QMenu *ChatEdit::createStandardContextMenu(const QPoint &position)
{
    QMenu *menu = QTextEdit::createStandardContextMenu(position);
    menu->addAction(actPasteAsQuote_);
    return menu;
}

bool ChatEdit::checkSpellingGloballyEnabled()
{
    return (SpellChecker::instance()->available() && PsiOptions::instance()->getOption(spellOption).toBool());
}

void ChatEdit::setCheckSpelling(bool b)
{
    document()->blockSignals(true);
    check_spelling_ = b;
    if (check_spelling_) {
        if (!spellhighlighter_)
            spellhighlighter_.reset(new SpellHighlighter(document()));
    } else {
        spellhighlighter_.reset();
    }
    document()->blockSignals(false);
}

bool ChatEdit::focusNextPrevChild(bool next) { return QWidget::focusNextPrevChild(next); }

// Qt text controls are quite greedy to grab key events.
// disable that.
bool ChatEdit::event(QEvent *event)
{
    if (event->type() == QEvent::ShortcutOverride) {
        return false;
    }
    if (event->type() == QEvent::PaletteChange && recButton_ && !correction) {
        setRecButtonIcon();
    }
    return QTextEdit::event(event);
}

void ChatEdit::keyPressEvent(QKeyEvent *e)
{
    /*    if(e->key() == Qt::Key_Escape || (e->key() == Qt::Key_W && e->modifiers() & Qt::ControlModifier))
        e->ignore();
    else if(e->key() == Qt::Key_Return &&
           ((e->modifiers() & Qt::ControlModifier)
#ifndef Q_OS_MAC
           || (e->modifiers() & Qt::AltModifier)
#endif
           ))
        e->ignore();
    else if(e->key() == Qt::Key_M && (e->modifiers() & Qt::ControlModifier)) // newline
        insert("\n");
    else if(e->key() == Qt::Key_H && (e->modifiers() & Qt::ControlModifier)) // history
        e->ignore();
    else  if(e->key() == Qt::Key_S && (e->modifiers() & Qt::AltModifier))
        e->ignore();
    else*/
    if (e->key() == Qt::Key_U && (e->modifiers() & Qt::ControlModifier))
        setText("");
/*    else if((e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter) && !((e->modifiers() & Qt::ShiftModifier) ||
   (e->modifiers() & Qt::AltModifier)) && LEGOPTS.chatSoftReturn) e->ignore(); else if((e->key() == Qt::Key_PageUp ||
   e->key() == Qt::Key_PageDown) && (e->modifiers() & Qt::ShiftModifier)) e->ignore(); else if((e->key() ==
   Qt::Key_PageUp || e->key() == Qt::Key_PageDown) && (e->modifiers() & Qt::ControlModifier)) e->ignore(); */
#ifdef Q_OS_MAC
    else if (e->key() == Qt::Key_QuoteLeft && e->modifiers() == Qt::ControlModifier) {
        e->ignore();
    }
#endif
    else {
        QTextEdit::keyPressEvent(e);
    }
}

/**
 * Work around Qt bug, that QTextEdit doesn't accept() the
 * event, so it could result in another context menu popping
 * out after the first one.
 */
void ChatEdit::contextMenuEvent(QContextMenuEvent *e)
{
    last_click_ = e->pos();
    if (check_spelling_ && textCursor().selectedText().isEmpty() && SpellChecker::instance()->available()) {
        // Check if the word under the cursor is misspelled
        QTextCursor tc = cursorForPosition(last_click_);
        tc.movePosition(QTextCursor::StartOfWord, QTextCursor::MoveAnchor);
        tc.movePosition(QTextCursor::EndOfWord, QTextCursor::KeepAnchor);
        QString selected_word = tc.selectedText();
        if (!selected_word.isEmpty() && !QRegExp("\\d+").exactMatch(selected_word)
            && !SpellChecker::instance()->isCorrect(selected_word)) {
            QList<QString> suggestions = SpellChecker::instance()->suggestions(selected_word);
            if (!suggestions.isEmpty() || SpellChecker::instance()->writable()) {
                QMenu spell_menu;
                if (!suggestions.isEmpty()) {
                    for (const QString &suggestion : suggestions) {
                        QAction *act_suggestion = spell_menu.addAction(suggestion);
                        connect(act_suggestion, &QAction::triggered, this, &ChatEdit::applySuggestion);
                    }
                    spell_menu.addSeparator();
                }
                if (SpellChecker::instance()->writable()) {
                    QAction *act_add = spell_menu.addAction(tr("Add to dictionary"));
                    connect(act_add, &QAction::triggered, this, &ChatEdit::addToDictionary);
                }
                spell_menu.exec(QCursor::pos());
                e->accept();
                return;
            }
        }
    }

    // Do custom menu
    QMenu *menu = createStandardContextMenu(e->pos());
    menu->exec(e->globalPos());
    delete menu;
    e->accept();
}

/*!
 * \brief handles a click on a suggestion
 * \param the action is just the container which holds the suggestion.
 *
 * This method is called by the framework whenever a user clicked on the child popupmenu
 * to select a suggestion for a missspelled word. It exchanges the missspelled word with the
 * suggestion which is the text of the QAction parameter.
 */
void ChatEdit::applySuggestion()
{
    auto *act_suggestion   = qobject_cast<QAction *>(sender());
    int   current_position = textCursor().position();

    // Replace the word
    QTextCursor tc = cursorForPosition(last_click_);
    tc.movePosition(QTextCursor::StartOfWord, QTextCursor::MoveAnchor);
    tc.movePosition(QTextCursor::EndOfWord, QTextCursor::KeepAnchor);
    int old_length = tc.position() - tc.anchor();
    tc.insertText(act_suggestion->text());
    tc.clearSelection();

    // Put the cursor where it belongs
    int new_length = act_suggestion->text().length();
    tc.setPosition(current_position - old_length + new_length);
    setTextCursor(tc);
}

/*!
 * \brief handles a click on the add2dict action of the parent popupmenu
 * \param Never used bool parameter
 *
 * The method sets the cursor to the last mouseclick position and looks for the word which is placed there.
 * This word is than added to the dictionary of aspell.
 */
void ChatEdit::addToDictionary()
{
    QTextCursor tc               = cursorForPosition(last_click_);
    int         current_position = textCursor().position();

    // Get the selected word
    tc.movePosition(QTextCursor::StartOfWord, QTextCursor::MoveAnchor);
    tc.movePosition(QTextCursor::EndOfWord, QTextCursor::KeepAnchor);
    SpellChecker::instance()->add(tc.selectedText());

    // Put the cursor where it belongs
    tc.clearSelection();
    tc.setPosition(current_position);
    setTextCursor(tc);
}

void ChatEdit::optionsChanged(const QString &option)
{
    if (option == spellOption)
        setCheckSpelling(checkSpellingGloballyEnabled());
    if (option == capOption)
        capitalizer_->setEnabled(PsiOptions::instance()->getOption(capOption).toBool());
    if (option == audioMessage) {
        bool isEnabled = PsiOptions::instance()->getOption(audioMessage).toBool();
        if (!recButton_ && isEnabled) {
            addSoundRecButton();
        } else if (recButton_ && !isEnabled) {
            removeSoundRecButton();
        }
    }
}

void ChatEdit::showHistoryMessageNext()
{
    correction = false;
    if (!typedMsgsHistory.isEmpty()) {
        if (typedMsgsIndex + 1 < typedMsgsHistory.size()) {
            ++typedMsgsIndex;
            showMessageHistory();
        } else {
            if (typedMsgsIndex != typedMsgsHistory.size()) {
                typedMsgsIndex = typedMsgsHistory.size();
                // Restore last typed text
                setEditText(currentText);
                updateBackground();
            }
        }
    }
}

void ChatEdit::changeActPasteAsQuoteState()
{
    QClipboard *clipboard = QApplication::clipboard();
    actPasteAsQuote_->setEnabled(clipboard->mimeData()->hasText());
}

void ChatEdit::pasteAsQuote()
{
    QString text = QApplication::clipboard()->mimeData()->text();
    insertAsQuote(text);
}

void ChatEdit::showHistoryMessagePrev()
{
    if (!typedMsgsHistory.isEmpty() && (typedMsgsIndex > 0 || correction)) {
        // Save current typed text
        if (typedMsgsIndex == typedMsgsHistory.size()) {
            currentText = toPlainText();
            correction  = true;
        }
        if (typedMsgsIndex == typedMsgsHistory.size() - 1 && correction) {
            correction = false;
            ++typedMsgsIndex;
        }
        --typedMsgsIndex;
        showMessageHistory();
    }
}

void ChatEdit::showHistoryMessageFirst()
{
    correction = false;
    if (!typedMsgsHistory.isEmpty()) {
        if (currentText.isEmpty()) {
            typedMsgsIndex = typedMsgsHistory.size() - 1;
            showMessageHistory();
        } else {
            typedMsgsIndex = typedMsgsHistory.size();
            // Restore last typed text
            setEditText(currentText);
            updateBackground();
        }
    }
}

void ChatEdit::showHistoryMessageLast()
{
    correction = false;
    if (!typedMsgsHistory.isEmpty()) {
        typedMsgsIndex = 0;
        showMessageHistory();
    }
}

void ChatEdit::setEditText(const QString &text)
{
    setPlainText(text);
    moveCursor(QTextCursor::End);
}

void ChatEdit::insertFromMimeData(const QMimeData *source)
{
    auto obtainSourceText = [source]() {
        if (!source->text().isEmpty())
            return source->text();
        return QString::fromLocal8Bit(source->data("text/plain"));
    };
    if (source->hasImage() || source->hasUrls()) {
        // Check that source doesn't contains a local files and paste data as a text
        bool        isLocalFile = false;
        const auto &urls        = source->urls();
        for (const QUrl &url : urls) {
            if (url.isLocalFile())
                isLocalFile = true;
        }
        if (source->hasText() && !isLocalFile) {
            textCursor().insertText(obtainSourceText());
            return;
        }
        emit fileSharingRequested(source);
        return;
    }
// dirty hacks to make text drag-n-drop work in OS Linux with Qt>=5.11
#if defined(Q_OS_LINUX) && (QT_VERSION >= QT_VERSION_CHECK(5, 11, 0))
    if (source->hasText()) {
        textCursor().insertText(obtainSourceText());
        return;
    }
    if (source->hasHtml()) {
        textCursor().insertText(TextUtil::rich2plain(source->html()));
        return;
    }
#endif
    QTextEdit::insertFromMimeData(source);
}

bool ChatEdit::canInsertFromMimeData(const QMimeData *source) const
{
    return (source->hasText() || source->hasHtml() || source->hasUrls() || source->hasImage()
            || QTextEdit::canInsertFromMimeData(source));
}

void ChatEdit::updateBackground()
{
    setProperty("correction", correction);
    style()->unpolish(this);
    style()->polish(this);
    update();
}

void ChatEdit::showMessageHistory()
{
    setEditText(typedMsgsHistory.at(typedMsgsIndex));
    updateBackground();
}

void ChatEdit::appendMessageHistory(const QString &text)
{
    if (!text.simplified().isEmpty()) {
        if (currentText == text)
            // Remove current typed text only if we want to add it to history
            currentText.clear();
        int index = typedMsgsHistory.indexOf(text);
        if (index >= 0) {
            typedMsgsHistory.removeAt(index);
        }
        if (typedMsgsHistory.size() >= MAX_MESSAGE_HISTORY) {
            typedMsgsHistory.removeAt(0);
        }
        typedMsgsHistory += text;
        typedMsgsIndex = typedMsgsHistory.size();
    }
}

void ChatEdit::clearMessageHistory()
{
    typedMsgsHistory.clear();
    typedMsgsIndex = 0;
}

XMPP::HTMLElement ChatEdit::toHTMLElement()
{
    XMPP::HTMLElement elem;
    QString           html      = toHtml();
    int               index     = html.indexOf("<body");
    int               lastIndex = html.lastIndexOf("</body>");
    if (index == -1 || lastIndex == -1)
        return elem;
    lastIndex += 7;
    html = html.mid(index, lastIndex - index);
    QDomDocument doc;
    if (!doc.setContent(html))
        return elem;
    QDomElement htmlElem = doc.firstChildElement("body");
    QDomElement p        = htmlElem.firstChildElement("p");
    if (p.isNull()) {
        // try Qt 5.15 way
        p = htmlElem.firstChildElement("table").firstChildElement("tr").firstChildElement("td").firstChildElement("p");
    }
    QDomElement body    = doc.createElementNS("http://www.w3.org/1999/xhtml", "body");
    bool        htmlish = false;
    while (!p.isNull()) {
        for (auto pc = p.firstChild(); !pc.isNull(); pc = pc.nextSibling()) {
            if (pc.isElement() && pc.toElement().tagName() != QStringLiteral("br"))
                htmlish = true;
            body.appendChild(pc.cloneNode(true));
        }
        p = p.nextSiblingElement("p");
    }
    if (htmlish && body.childNodes().size() > 0) {
        elem.setBody(body);
    }
    return elem;
}

void ChatEdit::doHTMLTextMenu() { controller_->doMenu(); }

void ChatEdit::setCssString(const QString &css) { controller_->setCssString(css); }

void ChatEdit::insertAsQuote(const QString &text)
{
    int     pos      = textCursor().position();
    QString prevLine = toPlainText().left(pos - 1);
    prevLine         = prevLine.mid(prevLine.lastIndexOf("\n") + 1);

    QString quote = QString::fromUtf8(u8"» ") + text;
    quote.replace("\n", QString::fromUtf8(u8"\n» "));

    // Check for previous quote and merge if true
    if (!prevLine.isEmpty() && !prevLine.startsWith(QString::fromUtf8(u8"»"))) {
        quote.prepend("\n");
    }
    quote.append("\n");
    insertPlainText(quote);
    setFocus(Qt::OtherFocusReason);
}

void ChatEdit::addSoundRecButton()
{
    if (!recButton_) {
        layout_    = new QHBoxLayout(this);
        recButton_ = new QToolButton(this);
        overlay_   = new QLabel(this);

        // Set text right margin for rec button
        connect(document(), &QTextDocument::contentsChanged, this, &ChatEdit::setRigthMargin);

        // Add text label and rec button to the right side of LineEdit
        // Setting label color to grey with 70% opacity with red bold text
        overlay_->setStyleSheet("background-color: rgba(169, 169, 169, 0.7); color: red; font-weight: bold;");
        overlay_->setAlignment(Qt::AlignCenter);
        setOverlayText(maxOverlayTime);
        overlay_->setVisible(false);
        layout_->addWidget(overlay_);
        recButton_->setToolTip(tr("Record and share audio note while pressed"));
        setRecButtonIcon();
        auto iconSize = fontInfo().pixelSize() * 1.5;
        recButton_->setMinimumSize(QSize(iconSize, iconSize));
        layout_->addWidget(recButton_);
        layout_->setAlignment(Qt::AlignRight | Qt::AlignBottom);

        connect(recButton_, &QToolButton::pressed, this, [this]() { // Rec button pressed
            if (recorder_) {
                recorder_->disconnect();
                recorder_.reset();
            }

            recorder_.reset(new AudioRecorder);
            recorder_->setMaxDuration(TIMEOUT);
            connect(recorder_.get(), &AudioRecorder::recorded, this, [this]() {
                if (recorder_->duration() < 1000)
                    return;

                QMimeData md;
                auto      data = recorder_->data();
                auto      mime = QMimeDatabase().mimeTypeForData(data).name();
                md.setData(mime, data);
                md.setData("application/x-psi-amplitudes", recorder_->amplitudes());
                emit fileSharingRequested(&md);
            });
            connect(recorder_.get(), &AudioRecorder::recordingStarted, this, [this]() {
                recButton_->setIcon(IconsetFactory::iconPixmap("psi/mic_rec", fontInfo().pixelSize() * 1.5));
                overlay_->setVisible(true);
                timeout_ = TIMEOUT;
                timer_   = new QTimer(this); // countdown timer to stop recording while the button is pressed
                connect(timer_, &QTimer::timeout, this, [this]() {
                    if (timeout_ > 0) {
                        timeout_ -= SECOND;
                        setOverlayText(timeout_ / SECOND);
                    } else {
                        timer_->stop();
                        recorder_->stop();
                    }
                });
                timer_->start(SECOND);
            });
            recorder_->record();
        });
        connect(recButton_, &QToolButton::released, this, [this]() { // Rec button relesed
            setRecButtonIcon();
            if (timer_) {
                timer_->stop();
                delete timer_;
            }
            setOverlayText(maxOverlayTime);
            overlay_->setVisible(false);
            if (recorder_) {
                recorder_->stop();
            }
        });
    }
}

void ChatEdit::removeSoundRecButton()
{
    delete recButton_;
    delete overlay_;
    delete layout_;
    disconnect(recorder_.get());
    recorder_.reset();
    disconnect(document(), &QTextDocument::contentsChanged, this, &ChatEdit::setRigthMargin);
    setRigthMargin();
}

void ChatEdit::setOverlayText(int value) { overlay_->setText(tr("Recording (%1 sec left)").arg(value)); }

void ChatEdit::setRecButtonIcon()
{
    auto isColorDark = [](int r, int g, int b) { return (r * 0.299 + g * 0.578 + b * 0.144) <= 186; };
    if (recButton_) {
        const QColor bcgColor(palette().color(backgroundRole()));
        int          red, green, blue = 0;
        bcgColor.getRgb(&red, &green, &blue);
        auto mis = fontInfo().pixelSize() * 1.5;
        if (isColorDark(red, green, blue)) {
            // Invert icon pixmap if background color is dark
            QImage recImage = IconsetFactory::icon("psi/mic").image(QSize(mis, mis));
            recImage.invertPixels();
            recButton_->setIcon(QPixmap::fromImage(recImage));
        } else {
            recButton_->setIcon(IconsetFactory::iconPixmap("psi/mic", mis));
        }
        const QColor toolTipBgColor(recButton_->palette().color(recButton_->backgroundRole()));
        toolTipBgColor.getRgb(&red, &green, &blue);
        const QString btnStyle
            = QString("border: 0; color: %1;").arg(isColorDark(red, green, blue) ? "white" : "black");
        recButton_->setStyleSheet(btnStyle);
    }
}

int ChatEdit::recButtonHeigth() const { return recButton_ ? recButton_->minimumHeight() : 0; }

void ChatEdit::setRigthMargin()
{
    // Set margin for text to avoid text placing under record button
    const float      margin = recButtonHeigth() * 1.5;
    QTextFrameFormat frmt   = document()->rootFrame()->frameFormat();
    if (frmt.rightMargin() < margin || margin == 0) {
        document()->blockSignals(true);
        frmt.setRightMargin(margin);
        document()->rootFrame()->setFrameFormat(frmt);
        document()->blockSignals(false);
    }
}

//----------------------------------------------------------------------------
// LineEdit
//----------------------------------------------------------------------------
LineEdit::LineEdit(QWidget *parent) : ChatEdit(parent)
{
    setWordWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere); // no need for horizontal scrollbar with this
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    setMinimumHeight(0);

    connect(document(), &QTextDocument::contentsChanged, this, &LineEdit::recalculateSize);
}

LineEdit::~LineEdit() { }

QSize LineEdit::minimumSizeHint() const
{
    const int spacing = fontMetrics().lineSpacing() + 1;
    const int sz      = hasSoundRecButton() ? qMax(recButtonHeigth() * 2 - 1, spacing) : spacing;
    QSize     sh      = QTextEdit::minimumSizeHint();
    sh.setHeight(sz);
    sh += QSize(0, QFrame::lineWidth() * 2);
    return sh;
}

QSize LineEdit::sizeHint() const
{
    QSize     sh      = QTextEdit::sizeHint();
    const int dHeight = int(document()->documentLayout()->documentSize().height());
    const int sz      = hasSoundRecButton() ? qMax(recButtonHeigth() * 2 - 1, dHeight) : dHeight;
    sh.setHeight(sz);
    sh += QSize(0, QFrame::lineWidth() * 2);
    static_cast<QTextEdit *>(const_cast<LineEdit *>(this))->setMaximumHeight(sh.height());
    return sh;
}

void LineEdit::resizeEvent(QResizeEvent *e)
{
    ChatEdit::resizeEvent(e);
    QTimer::singleShot(0, this, &LineEdit::updateScrollBar);
}

void LineEdit::recalculateSize()
{
    updateGeometry();
    QTimer::singleShot(0, this, &LineEdit::updateScrollBar);
}

void LineEdit::updateScrollBar()
{
    setVerticalScrollBarPolicy(sizeHint().height() > height() ? Qt::ScrollBarAlwaysOn : Qt::ScrollBarAlwaysOff);
    ensureCursorVisible();
}

#include "msgmle.moc"
