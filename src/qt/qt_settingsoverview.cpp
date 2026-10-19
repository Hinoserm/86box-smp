/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          The Overview of the slot-based Settings layout (preview): the
 *          machine as one tree.
 */
#include <QAbstractItemModel>
#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QWheelEvent>
#include <QDialog>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QEventLoop>
#include <QStyledItemDelegate>
#include <QStyleOptionComboBox>
#include <QPainter>
#include <QKeyEvent>
#include <QCursor>
#include <QFileInfo>
#include <QInputDialog>
#include <QLineEdit>
#include <QFocusEvent>
#include <QDebug>
#include <QSignalBlocker>
#include <QStandardItemModel>
#include <QLocale>
#include <QMap>
#include <QStyle>
#include <QMessageBox>
#include <QUrl>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPointer>
#include <QPushButton>
#include <QSet>
#include <QScrollBar>
#include <QSpinBox>
#include <QTabWidget>
#include <QTimer>
#include <QToolButton>
#include <QAbstractItemView>
#include <QTreeView>
#include <QTreeWidget>
#include <QApplication>
#include <QCompleter>
#include <QLineEdit>
#include <QTreeWidgetItemIterator>
#include <QVBoxLayout>

#include <algorithm>
#include <tuple>

extern "C" {
#include "../cpu/cpu.h"

#include <86box/86box.h>
#include <86box/device.h>
#include <86box/machine.h>
#include <86box/hdd.h>
#include <86box/timer.h>
#include <86box/cdrom.h>
#include <86box/fdd.h>
#include <86box/scsi_device.h>
#include <86box/scsi_tape.h>
#include <86box/mo.h>
#include <86box/rdisk.h>
#include <86box/hdc_ide.h>
#include <86box/scsi.h>
#include <86box/pci.h>
}

#include "qt_deviceconfig.hpp"
#include "qt_harddrive_common.hpp"
#include "qt_settings.hpp"
#include "qt_settingscards.hpp"
#include "qt_harddiskdialog.hpp"
#include "qt_settingsfloppycdrom.hpp"
#include "qt_settingsotherremovable.hpp"
#include "qt_settingsdrives.hpp"
#include "qt_settingsharddisks.hpp"
#include "qt_settingsoverview.hpp"

enum {
    ColumnDevice = 0,
    ColumnType,
    ColumnActions
};

namespace {
constexpr int DeferredEditorRole = Qt::UserRole + 90;
constexpr int CardControlRole = Qt::UserRole + 91;

/* Paint idle selectors without allocating native popup windows for every
   slot and drive. Clicking or activating the cell creates its editor. */
class BoardSelectorDelegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        if (!index.data(DeferredEditorRole).toBool()) {
            QStyledItemDelegate::paint(painter, option, index);
            return;
        }
        // Native combo backgrounds may be translucent. Paint the row's
        // background without its label, then let the combo draw it once.
        painter->save();
        painter->setClipRect(option.rect, Qt::IntersectClip);
        QStyleOptionViewItem background(option);
        initStyleOption(&background, index);
        background.text.clear();
        background.icon = QIcon();
        option.widget->style()->drawControl(QStyle::CE_ItemViewItem, &background, painter, option.widget);
        QStyleOptionComboBox combo;
        combo.initFrom(option.widget);
        combo.rect = option.rect.adjusted(0, 1, 0, -1);
        combo.currentText = index.data().toString();
        combo.state = QStyle::State_Enabled | QStyle::State_Active;
        combo.frame = true;
        option.widget->style()->drawComplexControl(QStyle::CC_ComboBox, &combo, painter, option.widget);
        option.widget->style()->drawControl(QStyle::CE_ComboBoxLabel, &combo, painter, option.widget);
        painter->restore();
    }
};
}

SettingsOverview::SettingsOverview(const Pages &pages, QWidget *parent)
    : QWidget(parent)
    , pages(pages)
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);

    tree = new QTreeWidget(this);
    tree->setColumnCount(3);
    tree->setHeaderLabels({ tr("Board / Slot"), tr("Installed device"), tr("Settings") });
    tree->header()->setStretchLastSection(false);
    tree->header()->setSectionResizeMode(ColumnType, QHeaderView::Stretch);
    tree->setUniformRowHeights(false);
    tree->setSelectionMode(QAbstractItemView::SingleSelection);
    tree->setFocusPolicy(Qt::StrongFocus);
    tree->installEventFilter(this);
    tree->setAlternatingRowColors(true);
    tree->setIndentation(18);
    tree->setColumnWidth(ColumnDevice, 255);
    tree->setColumnWidth(ColumnActions, 100);
    layout->addWidget(tree);
    tree->setItemDelegateForColumn(ColumnType, new BoardSelectorDelegate(tree));
    auto openEditor = [this](QTreeWidgetItem *item, int column, bool popup) {
        if (column != ColumnType || !deferredEditors.contains(item))
            return;
        materializeEditor(item);
        if (auto *cell = tree->itemWidget(item, ColumnType))
            if (auto *combo = cell->findChild<QComboBox *>()) {
                combo->setFocus(Qt::OtherFocusReason);
                if (popup || !combo->isEditable())
                    combo->showPopup();
                else if (combo->lineEdit())
                    combo->lineEdit()->selectAll();
            }
    };
    connect(tree, &QTreeWidget::itemClicked, this, [this, openEditor](QTreeWidgetItem *item, int column) {
        const int x = tree->viewport()->mapFromGlobal(QCursor::pos()).x();
        openEditor(item, column, x >= tree->columnViewportPosition(ColumnType) + tree->columnWidth(ColumnType) - 28);
    });
    connect(tree, &QTreeWidget::itemActivated, this, [openEditor](QTreeWidgetItem *item, int column) { openEditor(item, column, true); });

    Cards::follow(this, [this]() { refreshSoon(); });

    timer = new QTimer(this);
    timer->setSingleShot(true);
    timer->setInterval(0);
    connect(timer, &QTimer::timeout, this, [this]() { refresh(); });

    /* Whatever changes the classic pages' controls changes the tree. */
    for (auto *cbox : Settings::settings->findChildren<QComboBox *>()) {
        if (!pages.ports->isAncestorOf(cbox) && !pages.input->isAncestorOf(cbox))
            connect(cbox, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this]() { refreshSoon(); });
    }
    for (auto *check : Settings::settings->findChildren<QCheckBox *>()) {
        if (!pages.ports->isAncestorOf(check))
            connect(check, &QCheckBox::toggled, this, [this]() { refreshSoon(); });
    }
    for (auto *view : { pages.harddisks->findChild<QTreeView *>("treeView"), Settings::settings->findChild<QTreeView *>("treeViewCDROM"),
                        Settings::settings->findChild<QTreeView *>("treeViewFloppy") }) {
        if (view == nullptr)
            continue;
        connect(view->model(), &QAbstractItemModel::rowsInserted, this, [this]() { refreshSoon(); });
        connect(view->model(), &QAbstractItemModel::rowsRemoved, this, [this]() { refreshSoon(); });
        connect(view->model(), &QAbstractItemModel::dataChanged, this, [this]() { refreshSoon(); });
    }
}

/* The wheel over a drop-down scrolls the tree: it never chooses. A combo
   box takes the wheel as a choice, and a choice here can open a window. */
bool
SettingsOverview::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == tree && event->type() == QEvent::KeyPress) {
        auto *key = static_cast<QKeyEvent *>(event);
        auto *item = tree->currentItem();
        if (item && deferredEditors.contains(item) &&
            (key->key() == Qt::Key_Space || key->key() == Qt::Key_Return || key->key() == Qt::Key_F4 ||
             (key->key() == Qt::Key_Down && key->modifiers().testFlag(Qt::AltModifier)))) {
            materializeEditor(item);
            if (auto *cell = tree->itemWidget(item, ColumnType))
                if (auto *combo = cell->findChild<QComboBox *>()) {
                    combo->setFocus();
                    combo->showPopup();
                }
            return true;
        }
    }
    if ((event->type() == QEvent::Wheel) && (qobject_cast<QComboBox *>(watched) != nullptr)) {
        QCoreApplication::sendEvent(tree->viewport(), event);
        return true;
    }

    return QWidget::eventFilter(watched, event);
}

void
SettingsOverview::materializeEditor(QTreeWidgetItem *item)
{
    const auto create = deferredEditors.take(item);
    if (!create)
        return;
    materializingEditor = true;
    create();
    materializingEditor = false;
}

void
SettingsOverview::refreshSoon()
{
    if (rebuilding)
        return;
    dirty = true;
    if (isVisible())
        timer->start();
}

void
SettingsOverview::showEvent(QShowEvent *event)
{
    if (dirty)
        refresh();
    QWidget::showEvent(event);
}

/* A stable path for expansion and focus restoration. */
static QString
keyOf(QTreeWidgetItem *item)
{
    QStringList path;
    for (; item != nullptr; item = item->parent())
        path.prepend(item->text(0));

    return path.join('/');
}

/* One action from a row at a time, run once the drop-down or button that
   asked for it has finished with its click. */
static bool acting = false;

static void
later(QObject *context, const std::function<void()> &action)
{
    if (acting)
        return;
    acting = true;
    QTimer::singleShot(0, context, [action]() {
        action();
        acting = false;
    });
}

QTreeWidgetItem *
SettingsOverview::row(QTreeWidgetItem *parent, const QString &label, const QString &text)
{
    auto *container = parent ? parent : tree->invisibleRootItem();
    for (int i = 0; i < container->childCount(); ++i) {
        auto *item = container->child(i);
        if (!visitedRows.contains(item) && item->text(0) == label) {
            visitedRows.insert(item);
            item->setText(1, text);
            return item;
        }
    }
    int position = 0;
    for (int i = 0; i < container->childCount(); ++i)
        position += visitedRows.contains(container->child(i));
    auto *item = new QTreeWidgetItem(QStringList { label, text });
    container->insertChild(position, item);
    visitedRows.insert(item);
    newRows.insert(item);
    return item;
}

/* A drop-down that is a classic page's own: the same entries, and choosing
   one chooses it there. */
QComboBox *
SettingsOverview::mirror(QTreeWidgetItem *item, QComboBox *source)
{
    if (source == nullptr)
        return nullptr;

    if (auto *cell = tree->itemWidget(item, ColumnType); cell && cell->property("sourceCombo").value<QObject *>() == source) {
        if (auto *existing = cell->findChild<QComboBox *>()) {
            QSignalBlocker blocked(existing);
            existing->setCurrentIndex(source->currentIndex());
            existing->setEnabled(source->isEnabled());
            return existing;
        }
    }
    auto *cbox = new QComboBox(tree);
    cbox->setModel(source->model());
    cbox->setCurrentIndex(source->currentIndex());
    cbox->setEnabled(source->isEnabled());
    cbox->setMaxVisibleItems(30);
    connect(cbox, QOverload<int>::of(&QComboBox::activated), source, [source](int index) { source->setCurrentIndex(index); });
    connect(source, QOverload<int>::of(&QComboBox::currentIndexChanged), cbox, [source, cbox]() {
        QSignalBlocker blocked(cbox);
        cbox->setCurrentIndex(source->currentIndex());
        cbox->setEnabled(source->isEnabled());
    });
    putType(item, cbox);

    tree->itemWidget(item, ColumnType)->setProperty("sourceCombo", QVariant::fromValue<QObject *>(source));
    return cbox;
}

/* What a row is, as wide as it needs and no wider. */
void
SettingsOverview::putType(QTreeWidgetItem *item, QWidget *widget)
{
    item->setData(ColumnType, DeferredEditorRole, false);
    item->setText(ColumnType, QString());
    deferredEditors.remove(item);
    ++changedEditors;
    auto *cell   = new QWidget(tree);
    auto *layout = new QHBoxLayout(cell);

    layout->setContentsMargins(0, 0, 0, 0);
    widget->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    if (auto *cbox = qobject_cast<QComboBox *>(widget)) {
        cbox->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        cbox->setMinimumContentsLength(12);
        cbox->setFocusPolicy(Qt::StrongFocus);
        cbox->installEventFilter(this);
    }
    cell->setProperty("boardRow", keyOf(item));
    widget->setParent(cell);
    layout->addWidget(widget);
    tree->setItemWidget(item, ColumnType, cell);
}

/* Every row has Options: its settings in a window, or greyed out where
   there are none. */
void
SettingsOverview::configureButton(QTreeWidgetItem *item, const std::function<void()> &action, bool enabled)
{
    auto *button = qobject_cast<QToolButton *>(tree->itemWidget(item, ColumnActions));
    if (!button) {
        button = new QToolButton(tree);
        tree->setItemWidget(item, ColumnActions, button);
    }
    disconnect(button, &QToolButton::clicked, this, nullptr);
    button->setText(tr("Configure..."));
    button->setMinimumHeight(24);
    button->setEnabled(enabled && action);
    if (action)
        connect(button, &QToolButton::clicked, this, [this, action]() { later(this, action); });
}

/* Some of the classic pages' widgets in a window of their own, and back
   where they were when it closes: they save with the rest of Settings. */
void
SettingsOverview::inWindow(const QString &title, const QList<QWidget *> &widgets)
{
    QDialog dialog(this);
    auto   *layout = new QVBoxLayout(&dialog);

    dialog.setWindowTitle(title);
    for (auto *widget : widgets) {
        if (widget == nullptr)
            continue;
        Borrow::take(widget, &dialog);
        layout->addWidget(widget);
        widget->show();
    }

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);

    dialog.exec();

    for (auto *widget : widgets) {
        if (widget != nullptr)
            Borrow::giveBack(widget);
    }
    refreshSoon();
}

/* A card's options, as its Configure button gives them; a network card's
   connection is with it. */
void
SettingsOverview::configureCard(const Cards::Card &card)
{
    if (card.category != Cards::CATEGORY_NETWORK) {
        Cards::configure(card);
        return;
    }

    auto   *connection = Cards::control(QString("tabNet%1").arg(card.index + 1));
    QWidget holder;
    auto   *layout  = new QHBoxLayout(&holder);
    auto   *options = new QPushButton(tr("Card options..."), &holder);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(options);
    layout->addStretch();
    options->setEnabled(Cards::configurable(card));
    connect(options, &QPushButton::clicked, this, [card]() { Cards::configure(card); });

    inWindow(card.name, { connection, &holder });
}

/* Every processor the board takes, maker, family and speed in one list. */
void
SettingsOverview::processorCombo(QTreeWidgetItem *item)
{
    auto *family = pages.machine->findChild<QComboBox *>("comboBoxCPU");
    auto *speed  = pages.machine->findChild<QComboBox *>("comboBoxSpeed");
    if ((family == nullptr) || (speed == nullptr))
        return;

    const int mach    = Settings::settings->currentMachine();
    const int curFam  = family->currentData().toInt();
    const int curCpu  = speed->currentData().toInt();
    auto     *cbox    = new QComboBox(tree);
    int       current = -1;

    cbox->setMaxVisibleItems(30);
    for (int f = 0; cpu_families[f].package != 0; f++) {
        if (!cpu_family_is_eligible(&cpu_families[f], mach))
            continue;
        for (int c = 0; cpu_families[f].cpus[c].cpu_type != 0; c++) {
            if (!cpu_is_eligible(&cpu_families[f], c, mach))
                continue;
            if ((f == curFam) && (c == curCpu))
                current = cbox->count();
            cbox->addItem(QString("%1 %2 %3").arg(cpu_families[f].manufacturer, cpu_families[f].name, cpu_families[f].cpus[c].name),
                          (f << 8) | c);
        }
    }
    cbox->setCurrentIndex(current);
    connect(cbox, QOverload<int>::of(&QComboBox::activated), this, [cbox, family, speed](int index) {
        const int value = cbox->itemData(index).toInt();
        const int f     = family->findData(value >> 8);
        if (f < 0)
            return;
        family->setCurrentIndex(f);
        const int c = speed->findData(value & 0xff);
        if (c >= 0)
            speed->setCurrentIndex(c);
    });
    putType(item, cbox);
}

/* The common sizes of memory the board takes, and what it has. */
void
SettingsOverview::memoryCombo(QTreeWidgetItem *item)
{
    auto *spin = pages.machine->findChild<QSpinBox *>("spinBoxRAM");
    if (spin == nullptr)
        return;

    const int mach    = Settings::settings->currentMachine();
    const int minimum = machine_get_min_ram(mach);
    const int maximum = machine_get_max_ram(mach);
    const int step    = qMax(machine_get_ram_granularity(mach), 1);
    const int divisor = (step < 1024) ? 1 : 1024;
    const int current = spin->value() * divisor;

    QList<int> sizes;
    for (qint64 kb = 64; kb <= maximum; kb *= 2) {
        if ((kb >= minimum) && ((kb % step) == 0))
            sizes.append(static_cast<int>(kb));
        /* 640 KB, the most a PC could use below the adapters. */
        if ((kb == 512) && (640 >= minimum) && (640 <= maximum) && ((640 % step) == 0))
            sizes.append(640);
    }
    for (const int kb : { minimum, maximum, current }) {
        if ((kb > 0) && !sizes.contains(kb))
            sizes.append(kb);
    }
    std::sort(sizes.begin(), sizes.end());

    auto *cbox = new QComboBox(tree);
    for (const int kb : sizes) {
        if (kb >= (1024 * 1024))
            cbox->addItem(tr("%1 GB").arg(kb / (1024.0 * 1024.0)), kb);
        else if (kb >= 1024)
            cbox->addItem(tr("%1 MB").arg(kb / 1024.0), kb);
        else
            cbox->addItem(tr("%1 KB").arg(kb), kb);
    }
    cbox->setCurrentIndex(cbox->findData(current));
    cbox->setEnabled(minimum != maximum);
    connect(cbox, QOverload<int>::of(&QComboBox::activated), this, [cbox, spin, divisor](int index) {
        spin->setValue(cbox->itemData(index).toInt() / divisor);
    });
    putType(item, cbox);
}

/* A slot: what is in it, or empty, and the cards that fit it to choose
   from. The list is made when it is opened: an ISA slot takes hundreds. */
namespace {
class SlotCombo : public QComboBox {
public:
    using QComboBox::QComboBox;
    std::function<void(SlotCombo *)> fill;
    bool filled = false;
    void invalidate()
    {
        if (!filled)
            return;
        filled = false;
        QSignalBlocker blocked(this);
        const QString current = currentText();
        QPointer<QAbstractItemModel> previous = model();
        auto *replacement = new QStandardItemModel(this);
        replacement->appendRow(new QStandardItem(current));
        setModel(replacement);
        setCurrentIndex(0);
        if (previous && previous->parent() == this)
            previous->deleteLater();
    }

    void populate()
    {
        if (!filled && fill) {
            filled = true;
            fill(this);
            view()->setTextElideMode(Qt::ElideNone);
            view()->setMinimumWidth(qMin(800, view()->sizeHintForColumn(0) + 28));
        }
    }

    void showPopup() override
    {
        populate();
        QComboBox::showPopup();
    }

};
}

void
SettingsOverview::slotCombo(QTreeWidgetItem *item, const QString &bus, const QList<Cards::Card> &cards, int card, int slot)
{
    const bool hasCard = card >= 0;
    const Cards::Card current = hasCard ? cards[card] : Cards::Card {};
    const QString identity = QString("%1|%2|%3|%4|%5").arg(bus).arg(slot)
        .arg(current.control, current.name).arg(current.instance);
    if (auto *cell = tree->itemWidget(item, ColumnType)) {
        if (cell->property("slotIdentity").toString() == identity) {
            if (auto *existing = cell->findChild<QComboBox *>())
                static_cast<SlotCombo *>(existing)->invalidate();
            return;
        }
    }
    item->setData(ColumnType, CardControlRole, current.control);
    if (!hasCard && tree->itemWidget(item, ColumnActions))
        tree->removeItemWidget(item, ColumnActions);
    if (slotEditorReady && !materializingEditor && !tree->itemWidget(item, ColumnType)) {
        item->setText(ColumnType, hasCard ? current.name : tr("(empty)"));
        item->setData(ColumnType, DeferredEditorRole, true);
        deferredEditors.insert(item, [this, item, bus, cards, card, slot]() { slotCombo(item, bus, cards, card, slot); });
        if (hasCard)
            configureButton(item, [this, current]() { configureCard(current); },
                            Cards::configurable(current) || current.category == Cards::CATEGORY_NETWORK);
        return;
    }
    SlotCombo *cbox = nullptr;
    if (auto *cell = tree->itemWidget(item, ColumnType))
        cbox = dynamic_cast<SlotCombo *>(cell->findChild<QComboBox *>());
    const bool reused = cbox != nullptr;
    if (!cbox) {
        cbox = new SlotCombo(tree);
        cbox->setMaxVisibleItems(20);
        cbox->setEditable(true);
        cbox->setInsertPolicy(QComboBox::NoInsert);
        cbox->completer()->setCompletionMode(QCompleter::PopupCompletion);
        cbox->completer()->setFilterMode(Qt::MatchContains);
        cbox->completer()->setCaseSensitivity(Qt::CaseInsensitive);
        cbox->lineEdit()->setPlaceholderText(tr("Search compatible cards..."));
        connect(cbox->lineEdit(), &QLineEdit::textEdited, cbox, [cbox](const QString &text) {
            cbox->populate();
            cbox->completer()->setCompletionPrefix(text);
            cbox->completer()->complete();
        });
    }
    slotEditorReady = true;
    {
        QSignalBlocker blocked(cbox);
        cbox->clear();
        cbox->addItem(hasCard ? current.name : tr("(empty)"));
        cbox->filled = false;
    }
    if (slot >= 0)
        cbox->setToolTip(tr("The selected card will occupy this board slot."));
    else
        cbox->setToolTip(tr("Physical slot information is unavailable for this position."));
    cbox->fill = [this, bus, hasCard, current, slot](SlotCombo *combo) {
        disconnect(combo, QOverload<int>::of(&QComboBox::activated), this, nullptr);
        const auto list = Cards::choices(-1, bus, hasCard ? &current : nullptr);
        auto *model = new QStandardItemModel(combo);
        QList<QStandardItem *> entries;
        auto append = [&entries](const QString &text, int value) {
            auto *entry = new QStandardItem(text);
            entry->setData(value, Qt::UserRole);
            entries.append(entry);
        };
        if (!hasCard || Cards::removable(current))
            append(tr("(empty)"), -1);
        const int currentRow = hasCard ? entries.size() : 0;
        if (hasCard)
            append(current.name, -2);
        if (!hasCard && Cards::canAddBridge(slot))
            append(tr("PCI-to-PCI bridge (nine more PCI slots behind it)"), -3);
        for (int i = 0; i < list.size(); i++)
            append(QString("%1: %2").arg(Cards::categoryTitle(list[i].category), list[i].name), i);

        const QString typed = combo->currentText();
        model->appendColumn(entries);
        QPointer<QAbstractItemModel> previous = combo->model();
        combo->setModel(model);
        if (previous && previous != model && previous->parent() == combo)
            previous->deleteLater();
        combo->setCurrentIndex(currentRow);
        combo->setEditText(typed);
        connect(combo, QOverload<int>::of(&QComboBox::activated), this, [this, combo, list, hasCard, current, slot, currentRow](int index) {
            QPointer<QComboBox> guard = combo;
            const int           which = combo->itemData(index).toInt();
            if (which == -2)
                return;
            if (which == -3) {
                Cards::addBridge(slot);
                refreshSoon();
                return;
            }
            if (which == -1) {
                if (hasCard)
                    Cards::remove(current);
            } else if (!Cards::put(list[which], hasCard ? &current : nullptr, this, slot) && !guard.isNull())
                guard->setCurrentIndex(currentRow);
            refreshSoon();
        });
    };
    if (reused)
        ++changedEditors;
    else
        putType(item, cbox);

    tree->itemWidget(item, ColumnType)->setProperty("slotIdentity", identity);
    cbox->setProperty("cardControl", current.control);
    cbox->setProperty("slotBus", bus);
    if (hasCard)
        configureButton(item, [this, current]() { configureCard(current); },
                        Cards::configurable(current) || (current.category == Cards::CATEGORY_NETWORK));
}

/* The drives on an IDE board or SCSI bus, from the drive pages' lists: a
   drop-down of what is there, with a new or existing disk and every kind
   of drive that goes on the bus to put there, each set in its own window. */
void
SettingsOverview::drivesUnder(QTreeWidgetItem *parent, bool scsi, int bus)
{
    enum Kind {
        KIND_HARDDISK = 0,
        KIND_CDROM,
        KIND_MO,
        KIND_RDISK,
        KIND_TAPE
    };
    struct Drive {
        int     channel;
        QString text;
        int     kind;
    };
    /* Each kind but the hard disk: its list, where its type is in the
       list, how a drive of it is named, and its settings. ATAPI and SCSI
       are the same numbers for all of them. */
    struct KindInfo {
        const char *view;
        int         typeColumn;
        QString     title;
        const char *controls;
    };
    const QMap<int, KindInfo> kinds = {
        { KIND_CDROM, { "treeViewCDROM", 2, tr("CD-ROM drive"), "cdControls" } },
        { KIND_MO, { "treeViewMO", 1, tr("MO drive"), "moControls" } },
        { KIND_RDISK, { "treeViewRDisk", 1, tr("Removable disk drive"), "rdiskControls" } },
        { KIND_TAPE, { "treeViewTape", 1, tr("Tape drive"), "tapeControls" } },
    };
    const int busType = scsi ? CDROM_BUS_SCSI : CDROM_BUS_ATAPI;
    auto     *removable = Settings::settings->findChild<SettingsOtherRemovable *>();
    QList<Drive> drives;

    if (auto *view = pages.harddisks->findChild<QTreeView *>("treeView")) {
        auto *model = view->model();
        for (int r = 0; r < model->rowCount(); r++) {
            const int type    = model->index(r, 0).data(Qt::UserRole).toInt();
            const int channel = model->index(r, 0).data(Qt::UserRole + 1).toInt();
            if (scsi ? ((type == HDD_BUS_SCSI) && ((channel >> 4) == bus)) : ((type == HDD_BUS_IDE) && ((channel >> 1) == bus)))
                drives.append({ channel, tr("Hard disk: %1").arg(model->index(r, 1).data().toString()), KIND_HARDDISK });
        }
    }
    for (auto it = kinds.cbegin(); it != kinds.cend(); ++it) {
        auto *view = Settings::settings->findChild<QTreeView *>(it->view);
        if (view == nullptr)
            continue;
        auto *model = view->model();
        for (int r = 0; r < model->rowCount(); r++) {
            const int type    = model->index(r, 0).data(Qt::UserRole).toInt();
            const int channel = model->index(r, 0).data(Qt::UserRole + 1).toInt();
            if ((type == busType) && ((scsi ? (channel >> 4) : (channel >> 1)) == bus))
                drives.append({ channel, QString("%1: %2").arg(it->title, model->index(r, it->typeColumn).data().toString()), it.key() });
        }
    }

    /* A drive of a kind put on, taken off or chosen, by its page. */
    auto put = [this, removable, scsi](int kind, int channel) {
        const int bus = scsi ? CDROM_BUS_SCSI : CDROM_BUS_ATAPI;
        if (kind == KIND_CDROM)
            return pages.floppyCdrom->addCdrom(bus, channel);
        return (removable != nullptr) && removable->addDrive(kind - KIND_MO, bus, channel);
    };
    auto takeOff = [this, removable, scsi](int kind, int channel) {
        const int bus = scsi ? CDROM_BUS_SCSI : CDROM_BUS_ATAPI;
        if (kind == KIND_HARDDISK)
            pages.harddisks->removeDrive(scsi ? HDD_BUS_SCSI : HDD_BUS_IDE, channel);
        else if (kind == KIND_CDROM)
            pages.floppyCdrom->removeCdrom(bus, channel);
        else if (removable != nullptr)
            removable->removeDrive(kind - KIND_MO, bus, channel);
    };
    auto choose = [this, removable, scsi](int kind, int channel) {
        const int bus = scsi ? CDROM_BUS_SCSI : CDROM_BUS_ATAPI;
        if (kind == KIND_HARDDISK)
            pages.harddisks->selectDrive(scsi ? HDD_BUS_SCSI : HDD_BUS_IDE, channel);
        else if (kind == KIND_CDROM)
            pages.floppyCdrom->selectCdrom(bus, channel);
        else if (removable != nullptr)
            removable->selectDrive(kind - KIND_MO, bus, channel);
    };

    auto driveRow = [this, scsi, kinds, put, takeOff, choose](QTreeWidgetItem *item, int channel, const Drive *drive) {
        const bool    there = (drive != nullptr);
        const int     kind  = there ? drive->kind : -1;
        const QString identity = QString("%1|%2|%3|%4").arg(scsi).arg(channel).arg(kind).arg(there ? drive->text : QString());
        if (auto *cell = tree->itemWidget(item, ColumnType))
            if (cell->property("driveIdentity").toString() == identity)
                return;
        const QString text = there ? drive->text : tr("(empty)");

        configureButton(item, [this, kind, channel, kinds, choose]() {
            choose(kind, channel);
            SettingsDrives::refreshChannelNames();
            if (kind == KIND_HARDDISK)
                inWindow(tr("Hard disk"), { Settings::settings->findChild<QWidget *>("hdControls") });
            else
                inWindow(kinds[kind].title, { Settings::settings->findChild<QWidget *>(kinds[kind].controls) });
        }, there);
        auto create = [this, item, channel, there, kind, text, identity, kinds, put, takeOff, scsi]() {
            auto *cbox = new QComboBox(tree);

            cbox->addItem(text);
            if (there)
                cbox->addItem(tr("(empty)"), 0);
            cbox->addItem(tr("New hard disk..."), 1);
            cbox->addItem(tr("Existing hard disk image..."), 2);
            for (auto it = kinds.cbegin(); it != kinds.cend(); ++it)
                cbox->addItem(it->title, 10 + it.key());

            connect(cbox, QOverload<int>::of(&QComboBox::activated), this, [this, cbox, channel, there, kind, kinds, put, takeOff, scsi](int index) {
                const int what = (index == 0) ? -1 : cbox->itemData(index).toInt();
                cbox->setCurrentIndex(0);
                if (what < 0)
                    return;

                later(this, [this, what, channel, there, kind, kinds, put, takeOff, scsi]() {
                    /* A new or existing disk replaces what is there only once
                       it is chosen: cancelling its window changes nothing. */
                    auto makeRoom = [there, kind, channel, takeOff]() {
                        if (there)
                            takeOff(kind, channel);
                    };
                    if ((what == 1) || (what == 2))
                        pages.harddisks->addDrive(what == 2, scsi ? HDD_BUS_SCSI : HDD_BUS_IDE, channel, makeRoom);
                    else {
                        makeRoom();
                        if ((what >= 10) && !put(what - 10, channel))
                            QMessageBox::information(this, kinds[what - 10].title,
                                                     tr("Every %1 there can be is in use.").arg(kinds[what - 10].title.toLower()));
                    }
                    refreshSoon();
                });
            });
            putType(item, cbox);
            tree->itemWidget(item, ColumnType)->setProperty("driveIdentity", identity);
        };
        if (!tree->itemWidget(item, ColumnType)) {
            item->setText(ColumnType, text);
            item->setData(ColumnType, DeferredEditorRole, true);
            deferredEditors.insert(item, create);
        } else
            create();
    };

    if (scsi) {
        /* A SCSI bus shows what is on it, not all sixteen IDs; a new device
           takes the lowest free ID, leaving 7 to the controller. */
        std::sort(drives.begin(), drives.end(), [](const Drive &a, const Drive &b) { return a.channel < b.channel; });
        QSet<int> used;
        for (const auto &drive : drives) {
            used.insert(drive.channel & 15);
            driveRow(row(parent, tr("SCSI ID %1").arg(drive.channel & 15)), drive.channel, &drive);
        }
        int id = 0;
        while ((id < 16) && (used.contains(id) || (id == 7)))
            id++;
        if (id < 16)
            driveRow(row(parent, tr("Add device")), (bus << 4) | id, nullptr);
        return;
    }

    for (int unit = 0; unit < 2; unit++) {
        const Drive *there = nullptr;
        for (const auto &drive : drives) {
            if ((drive.channel & 1) == unit)
                there = &drive;
        }
        driveRow(row(parent, tr("IDE %1:%2").arg(bus).arg(unit)), (bus << 1) | unit, there);
    }
}

void
SettingsOverview::refresh()
{
    if (rebuilding || !dirty)
        return;
    rebuilding = true;
    dirty = false;
    timer->stop();
    const int        mach  = Settings::settings->currentMachine();
    const machine_t *m     = &machines[mach];
    Cards::nameOnboardEntries();
    const auto       cards = Cards::list();
    const auto       nodes = Cards::layout(cards);

    /* Preserve expansion, focus and scroll while reconciling changed rows. */
    const int     scroll = tree->verticalScrollBar()->value();
    QSet<QString> collapsed;
    for (QTreeWidgetItemIterator it(tree); *it != nullptr; ++it) {
        if (((*it)->childCount() > 0) && !(*it)->isExpanded())
            collapsed.insert(keyOf(*it));
    }
    const bool first = (tree->topLevelItemCount() == 0);

    QString focusKey;
    if (auto *focus = QApplication::focusWidget(); focus && tree->isAncestorOf(focus)) {
        for (auto *widget = focus; widget && widget != tree; widget = widget->parentWidget()) {
            if (widget->property("boardRow").isValid()) {
                focusKey = widget->property("boardRow").toString();
                break;
            }
        }
    }
    tree->setUpdatesEnabled(false);
    const bool machineChanged = renderedMachine != mach;
    if (machineChanged) {
        tree->clear();
        slotEditorReady = false;
    }
    renderedMachine = mach;
    deferredEditors.clear();
    visitedRows.clear();
    newRows.clear();
    changedEditors = 0;

    auto deviceOptions = [this](QTreeWidgetItem *item, const device_t *dev) {
        configureButton(item, [dev]() { DeviceConfig::ConfigureDevice(dev); }, dev && dev->config);
    };
    auto *root = row(nullptr, tr("System board"), QString::fromUtf8(m->name));
    QFont heading = tree->font();
    heading.setBold(true);
    root->setFont(0, heading);

    /* The board's own devices. */
    auto *onboard = row(root, tr("Onboard devices"));
    if (const device_t *board = machine_get_device(mach))
        deviceOptions(onboard, board);

    char chip[512];
    machine_get_fdc_chip_name(mach, chip, sizeof(chip));
    QTreeWidgetItem *sio = onboard;
    if (m->sio_device != nullptr)
        sio = row(onboard, tr("Super I/O"), QString(chip));

    /* The floppy controller is always there to be chosen. */
    auto *fdc = row(sio, tr("Floppy controller"));
    mirror(fdc, qobject_cast<QComboBox *>(Cards::control("comboBoxFD")));
    configureButton(fdc, {}, false);
    for (const auto &card : cards) {
        if (card.category == Cards::CATEGORY_FDC)
            configureButton(fdc, [card]() { Cards::configure(card); }, Cards::configurable(card));
    }
    /* The floppy drives go under the controller that has them: the board's,
       or the card's in its slot, once the slots are made. */
    auto floppyDrives = [&](QTreeWidgetItem *fdc) {
        auto *view = Settings::settings->findChild<QTreeView *>("treeViewFloppy");
        if (view == nullptr)
            return;
        /* Each controller's drives, numbered on it as many as it has
           connectors for; a drive set up where no controller has a
           connector is shown as such. */
        auto      *model    = view->model();
        const auto floppies = Cards::floppyControllers();
        QHash<int, QTreeWidgetItem *> controllerItems;
        for (int r = 0; r < model->rowCount(); r++) {
            int       number     = 0;
            const int controller = Cards::floppyControllerOf(floppies, r, &number);
            const bool there     = model->index(r, 0).data(Qt::UserRole).toInt() != 0;
            if ((controller < 0) && !there)
                continue;
            QTreeWidgetItem *on = fdc;
            if (controller > 0) {
                if (!controllerItems.contains(controller))
                    controllerItems.insert(controller, row(fdc, tr("Secondary controller")));
                on = controllerItems[controller];
            }
            auto *drive = (controller >= 0) ? row(on, tr("Drive %1").arg(number))
                                            : row(fdc, tr("Drive with no connector (%1)").arg(r + 1));
            if (controller < 0)
                drive->setIcon(0, style()->standardIcon(QStyle::SP_MessageBoxWarning));
            if (auto *types = Settings::settings->findChild<QComboBox *>("comboBoxFloppyType")) {
                if (auto *cell = tree->itemWidget(drive, ColumnType)) {
                    if (auto *existing = cell->findChild<QComboBox *>()) {
                        QSignalBlocker blocked(existing);
                        existing->setCurrentIndex(model->index(r, 0).data(Qt::UserRole).toInt());
                        continue;
                    }
                }
                /* The drive's type, the same list as the page's. */
                auto *cbox = new QComboBox(tree);
                cbox->setModel(types->model());
                cbox->setCurrentIndex(model->index(r, 0).data(Qt::UserRole).toInt());
                cbox->setMaxVisibleItems(30);
                connect(cbox, QOverload<int>::of(&QComboBox::activated), this, [this, r](int index) {
                    later(this, [this, r, index]() { pages.floppyCdrom->setFloppyDriveType(r, index); });
                });
                putType(drive, cbox);
            } else
                drive->setText(ColumnType, model->index(r, 0).data().toString());
            configureButton(drive, [this, r]() {
                pages.floppyCdrom->selectFloppy(r);
                inWindow(tr("Floppy drive"), { Settings::settings->findChild<QWidget *>("floppyControls") });
            });
        }
    };

    for (int n = 1; n <= 4; n++) {
        for (const auto &[label, check, combo, button] :
             { std::tuple<QString, QString, QString, QString> { tr("COM%1").arg(n), QString("checkBoxSerial%1").arg(n), QString("comboBoxCom%1").arg(n),
                                                               QString("pushButtonConfigureCom%1").arg(n) },
               std::tuple<QString, QString, QString, QString> { tr("LPT%1").arg(n), QString("checkBoxParallel%1").arg(n), QString("comboBoxLpt%1").arg(n),
                                                               QString("pushButtonConfigureLpt%1").arg(n) } }) {
            auto *enable = pages.ports->findChild<QCheckBox *>(check);
            auto *device = pages.ports->findChild<QComboBox *>(combo);
            auto *conf   = pages.ports->findChild<QPushButton *>(button);
            if ((enable == nullptr) || (device == nullptr))
                continue;
            /* The board's own ports; the others are cards in slots. */
            const bool serial = check.startsWith("checkBoxSerial");
            if (n > (serial ? machine_get_serial_ports(mach) : machine_get_parallel_ports(mach)))
                continue;

            /* What is on the port, or Disabled; Options is its device's own
               settings. The input devices on it say so. */
            auto *item = row(sio, label);
            if (serial) {
                const auto users = Cards::portUsers(n);
                item->setText(0, users.isEmpty() ? label : QString("%1 (%2)").arg(label, users.join(", ")));
                item->setToolTip(0, users.isEmpty() ? QString() : tr("Chosen in the input device's own options."));
            }
            if (tree->itemWidget(item, ColumnType))
                continue;
            auto *cbox = new QComboBox(tree);
            cbox->setMaxVisibleItems(30);
            cbox->addItem(tr("Disabled"), -1);
            for (int i = 0; i < device->count(); i++)
                cbox->addItem(device->itemText(i), i);
            cbox->setCurrentIndex(enable->isChecked() ? (device->currentIndex() + 1) : 0);
            cbox->setEnabled(enable->isEnabled());
            connect(cbox, QOverload<int>::of(&QComboBox::activated), this, [cbox, enable, device](int index) {
                const int which = cbox->itemData(index).toInt();
                enable->setChecked(which >= 0);
                if (which >= 0)
                    device->setCurrentIndex(which);
            });
            putType(item, cbox);
            configureButton(item, [conf]() { conf->click(); }, (conf != nullptr) && enable->isChecked() && conf->isEnabled());
            auto *options = qobject_cast<QToolButton *>(tree->itemWidget(item, ColumnActions));
            auto sync = [cbox, device, enable, conf, options]() {
                QSignalBlocker blocked(cbox);
                cbox->setCurrentIndex(enable->isChecked() ? device->currentIndex() + 1 : 0);
                cbox->setEnabled(enable->isEnabled());
                options->setEnabled(conf && enable->isChecked() && conf->isEnabled());
            };
            connect(device, QOverload<int>::of(&QComboBox::currentIndexChanged), cbox, [cbox, sync]() { QTimer::singleShot(0, cbox, sync); });
            connect(enable, &QCheckBox::toggled, cbox, [cbox, sync]() { QTimer::singleShot(0, cbox, sync); });
        }
    }

    const device_t *kbc = machine_get_kbc_device(mach);
    deviceOptions(row(onboard, tr("Keyboard controller"), (kbc != nullptr) ? DeviceConfig::DeviceName(kbc, nullptr, 0) : QString()), kbc);
    if (const device_t *nvr = machine_get_nvr_device(mach))
        deviceOptions(row(onboard, tr("RTC/NVR"), DeviceConfig::DeviceName(nvr, nullptr, 0)), nvr);

    /* Who has each IDE board and SCSI bus, to hang the drives under. */
    bus_owner_t ideOwners[IDE_BUS_MAX];
    bus_owner_t scsiOwners[SCSI_BUS_MAX];
    const int   ideCount  = Harddrives::idePlan(ideOwners);
    const int   scsiCount = Harddrives::scsiPlan(scsiOwners);

    QHash<int, QTreeWidgetItem *> cardItems;
    QTreeWidgetItem              *boardIde = nullptr;

    /* The machine's own cards that have no device of their own in the
       board's layout. */
    QSet<int> onNodes;
    for (const auto &node : nodes) {
        if (node.card >= 0)
            onNodes.insert(node.card);
    }
    for (int i = 0; i < cards.size(); i++) {
        if (!cards[i].onboard || onNodes.contains(i) || (cards[i].category == Cards::CATEGORY_FDC))
            continue;
        const auto card = cards[i];
        auto      *item = row(onboard, Cards::categoryTitle(card.category));
        if (!card.check)
            mirror(item, qobject_cast<QComboBox *>(Cards::control(card.control)));
        else
            item->setText(ColumnType, card.name);
        configureButton(item, [this, card]() { configureCard(card); }, Cards::configurable(card) || (card.category == Cards::CATEGORY_NETWORK));
        cardItems.insert(i, item);
        if (card.category == Cards::CATEGORY_HDC)
            boardIde = item;
    }

    /* The buses, bridges and slots, with the cards in them. */
    QList<QTreeWidgetItem *> made;
    for (const auto &node : nodes) {
        /* The board's own chips are with its other devices, though they
           sit on the PCI bus: the bus keeps its bridges and slots. */
        const bool chip = (node.kind == Cards::Node::DEVICE) && (node.pciType != PCI_CARD_NORTHBRIDGE);
        auto      *parent = chip ? onboard : ((node.parent >= 0) ? made[node.parent] : root);
        QString    label  = node.label;
        if (chip) {
            if (label.startsWith("On-board ")) {
                label = label.mid(9);
                label[0] = label[0].toUpper();
            }
            label.replace("(device ", "(PCI device ");
        }
        auto *item = row(parent, label);
        made.append(item);

        if (node.kind == Cards::Node::SLOT) {
            slotCombo(item, node.bus, cards, node.card, node.slot);
            if (node.card >= 0)
                cardItems.insert(node.card, item);
        } else if (node.bridge > 0) {
            /* A bridge put in by hand: there, or taken out. */
            const QString identity = QString("bridge|%1|%2").arg(node.bridge).arg(node.slot);
            if (auto *cell = tree->itemWidget(item, ColumnType); !cell || (cell->property("bridgeIdentity").toString() != identity)) {
                auto *cbox = new QComboBox(tree);
                cbox->addItem(DeviceConfig::DeviceName(&dec21150_device, dec21150_device.internal_name, 0));
                cbox->addItem(tr("(empty) - take the bridge out"));
                const int n = node.bridge;
                connect(cbox, QOverload<int>::of(&QComboBox::activated), this, [this, cbox, n](int index) {
                    cbox->setCurrentIndex(0);
                    if (index == 1)
                        later(this, [this, n]() {
                            Cards::removeBridge(n);
                            refreshSoon();
                        });
                });
                putType(item, cbox);
                tree->itemWidget(item, ColumnType)->setProperty("bridgeIdentity", identity);
                configureButton(item, {}, false);
            }
        } else if ((node.kind == Cards::Node::DEVICE) && (node.card >= 0)) {
            const auto card = cards[node.card];
            if (!card.check)
                mirror(item, qobject_cast<QComboBox *>(Cards::control(card.control)));
            configureButton(item, [this, card]() { configureCard(card); }, Cards::configurable(card) || (card.category == Cards::CATEGORY_NETWORK));
            cardItems.insert(node.card, item);
            if (card.category == Cards::CATEGORY_HDC)
                boardIde = item;
        } else if ((node.kind == Cards::Node::DEVICE) && (node.pciType == PCI_CARD_SCSI) && (machine_get_scsi_device(mach) != nullptr)) {
            item->setText(ColumnType, DeviceConfig::DeviceName(machine_get_scsi_device(mach), nullptr, 0));
            deviceOptions(item, machine_get_scsi_device(mach));
        }
    }

    /* The board's own devices not in use, to be turned back on: on the
       board's PCI device of the kind where it has one. */
    for (const int category : { Cards::CATEGORY_VIDEO, Cards::CATEGORY_SOUND, Cards::CATEGORY_NETWORK, Cards::CATEGORY_HDC }) {
        Cards::Choice own;
        Cards::Card   displaced;
        if (!Cards::onboardChoice(category, &own, &displaced))
            continue;
        const bool replaces = !displaced.control.isEmpty();
        static const QMap<int, int> types = { { Cards::CATEGORY_VIDEO, PCI_CARD_VIDEO }, { Cards::CATEGORY_SOUND, PCI_CARD_SOUND },
                                              { Cards::CATEGORY_NETWORK, PCI_CARD_NETWORK }, { Cards::CATEGORY_HDC, PCI_CARD_IDE } };
        QTreeWidgetItem *item = nullptr;
        for (int n = 0; (n < nodes.size()) && (item == nullptr); n++) {
            if ((nodes[n].kind == Cards::Node::DEVICE) && (nodes[n].card < 0) && (nodes[n].pciType == types.value(category)))
                item = made[n];
        }
        if (item == nullptr)
            item = row(onboard, Cards::categoryTitle(category));
        const QString offer = replaces ? tr("%1 (in place of %2)").arg(own.name, displaced.name) : own.name;
        if (auto *cell = tree->itemWidget(item, ColumnType); cell && cell->property("onboardChoice").toString() == offer)
            continue;
        auto *cbox = new QComboBox(tree);
        cbox->addItem(tr("(not in use)"));
        cbox->addItem(offer);
        connect(cbox, QOverload<int>::of(&QComboBox::activated), this, [this, cbox, own, displaced, replaces](int index) {
            cbox->setCurrentIndex(0);
            if (index == 1)
                later(this, [this, own, displaced, replaces]() {
                    Cards::put(own, replaces ? &displaced : nullptr, this);
                    refreshSoon();
                });
        });
        putType(item, cbox);
        tree->itemWidget(item, ColumnType)->setProperty("onboardChoice", offer);
        configureButton(item, {}, false);
    }
    {
        QTreeWidgetItem *owner = fdc;
        for (int i = 0; i < cards.size(); i++) {
            if ((cards[i].category == Cards::CATEGORY_FDC) && !cards[i].onboard && cardItems.contains(i))
                owner = cardItems[i];
        }
        floppyDrives(owner);
    }
    /* The item a bus owner is: the card, or the board. */
    auto ownerItem = [&](const bus_owner_t &owner) -> QTreeWidgetItem * {
        if (owner.device == nullptr) {
            if (boardIde == nullptr)
                boardIde = row(onboard, tr("IDE"), tr("%1 IDE").arg(Cards::chipsetName(mach)));
            return boardIde;
        }
        for (int i = 0; i < cards.size(); i++) {
            if ((cards[i].dev == owner.device) && (cards[i].instance == owner.instance) && cardItems.contains(i))
                return cardItems[i];
        }
        for (int i = 0; i < cards.size(); i++) {
            if ((cards[i].dev == owner.device) && cardItems.contains(i))
                return cardItems[i];
        }
        /* A chip on the board that is not a card: the board's SCSI, say. */
        if (owner.device == machine_get_scsi_device(mach)) {
            for (int n = 0; n < nodes.size(); n++) {
                if ((nodes[n].kind == Cards::Node::DEVICE) && (nodes[n].pciType == PCI_CARD_SCSI))
                    return made[n];
            }
        }
        auto *item = row(onboard, owner.onboard ? tr("On-board controller") : tr("Controller"), DeviceConfig::DeviceName(owner.device, nullptr, 0));
        deviceOptions(item, owner.device);
        return item;
    };
    for (int b = 0; b < ideCount; b++) {
        if ((ideOwners[b].device != nullptr) || ideOwners[b].onboard)
            drivesUnder(ownerItem(ideOwners[b]), false, b);
    }
    for (int b = 0; b < scsiCount; b++) {
        if ((scsiOwners[b].device != nullptr) || scsiOwners[b].onboard) {
            auto *owner = ownerItem(scsiOwners[b]);
            drivesUnder((scsiCount > 1) ? row(owner, tr("SCSI bus %1").arg(b)) : owner, true, b);
        }
    }

    /* The devices a person works it with. */
    auto *input = row(root, tr("Input devices"));
    configureButton(input, [this]() { inWindow(tr("Input devices"), { pages.input }); });
    for (const auto &[label, name, button] : { std::tuple<QString, QString, QString> { tr("Keyboard"), "comboBoxKeyboard", "pushButtonConfigureKeyboard" },
                                               std::tuple<QString, QString, QString> { tr("Mouse"), "comboBoxMouse", "pushButtonConfigureMouse" },
                                               std::tuple<QString, QString, QString> { tr("Tablet"), "comboBoxTablet", "pushButtonConfigureTablet" },
                                               std::tuple<QString, QString, QString> { tr("Joystick (game port 1)"), "comboBoxJoystick0", "pushButtonJoystick01" },
                                               std::tuple<QString, QString, QString> { tr("Joystick (game port 2)"), "comboBoxJoystick1", "pushButtonJoystick11" },
                                               std::tuple<QString, QString, QString> { tr("Game port card"), "comboBoxGameportCard", "pushButtonConfigureGameportCard" } }) {
        auto *source = pages.input->findChild<QComboBox *>(name);
        if (source == nullptr)
            continue;
        auto *item = row(input, label);
        mirror(item, source);
        auto *conf = pages.input->findChild<QPushButton *>(button);
        configureButton(item, [conf]() { conf->click(); }, (conf != nullptr) && conf->isEnabled());
    }

    // Structural rows remain quiet; card and device rows carry their actions.
    // Remove only rows no longer represented, from leaves toward the root.
    QList<QTreeWidgetItem *> obsolete;
    for (QTreeWidgetItemIterator it(tree); *it; ++it)
        if (!visitedRows.contains(*it))
            obsolete.prepend(*it);
    for (auto *item : obsolete)
        delete item;
    for (auto *item : newRows)
        item->setExpanded(true);
    for (QTreeWidgetItemIterator it(tree); *it != nullptr; ++it) {
        if (collapsed.contains(keyOf(*it)))
            (*it)->setExpanded(false);
    }
    if (first) {
        onboard->setExpanded(false);
        input->setExpanded(false);
        for (auto *item : cardItems) {
            if (item->childCount())
                item->setExpanded(false);
        }
    }
    for (QTreeWidgetItemIterator it(tree); *it; ++it) {
        const auto key = keyOf(*it);
        if (auto *cell = tree->itemWidget(*it, ColumnType)) {
            cell->setProperty("boardRow", key);
            if (key == focusKey)
                if (auto *combo = cell->findChild<QComboBox *>())
                    combo->setFocus(Qt::OtherFocusReason);
        }
    }
    tree->doItemsLayout();
    tree->verticalScrollBar()->setValue(scroll);
    tree->setUpdatesEnabled(true);
    Cards::prunePanels();
    rebuilding = false;
}

namespace DriveImages {
/* A rename (to the new path) or a deletion (to empty) of an image file,
   made only when the Settings are saved. */
struct Change {
    QString from;
    QString to;
};

static QList<Change>           changes;
static QHash<QString, QString> usedAtStart; /* what the machine used when the dialog opened */

/* An image path as one file: without the wp:// or ioctl:// that a drive's
   path may carry, taken in the machine's directory when relative, and the
   file's canonical path where it exists. */
static QString
imageKey(const QString &path)
{
    QString name = path;

    for (const QString &prefix : { QStringLiteral("wp://"), QStringLiteral("ioctl://") }) {
        if (name.startsWith(prefix))
            name = name.mid(prefix.length());
    }
    if (name.isEmpty())
        return QString();

    const QFileInfo info(QDir(QString::fromUtf8(usr_path)).absoluteFilePath(name));
    return info.exists() ? info.canonicalFilePath() : info.absoluteFilePath();
}

static void
use(QHash<QString, QString> &users, const QString &path, const QString &who)
{
    const QString key = imageKey(path);

    if (!key.isEmpty())
        users.insert(key, who);
}

/* The images the configuration in memory uses: the machine's as it runs,
   or as just saved. */
static QHash<QString, QString>
configuredUses()
{
    QHash<QString, QString> users;

    for (int i = 0; i < HDD_NUM; i++) {
        if ((hdd[i].bus_type != HDD_BUS_DISABLED) && hdd[i].fn[0]) {
            use(users, QString::fromUtf8(hdd[i].fn), QCoreApplication::translate("SettingsDriveImages", "Hard disk %1").arg(i + 1));
            if (hdd[i].vhd_parent[0])
                use(users, QString::fromUtf8(hdd[i].vhd_parent),
                    QCoreApplication::translate("SettingsDriveImages", "Parent of hard disk %1").arg(i + 1));
        }
    }
    for (int i = 0; i < FDD_NUM; i++)
        use(users, QString::fromUtf8(drives[i].image_path), QCoreApplication::translate("SettingsDriveImages", "Floppy drive %1").arg(i + 1));
    for (int i = 0; i < CDROM_NUM; i++)
        use(users, QString::fromUtf8(cdrom[i].image_path), QCoreApplication::translate("SettingsDriveImages", "CD-ROM drive %1").arg(i + 1));
    for (int i = 0; i < RDISK_NUM; i++)
        use(users, QString::fromUtf8(rdisk_drives[i].image_path),
            QCoreApplication::translate("SettingsDriveImages", "Removable disk drive %1").arg(i + 1));
    for (int i = 0; i < MO_NUM; i++)
        use(users, QString::fromUtf8(mo_drives[i].image_path), QCoreApplication::translate("SettingsDriveImages", "MO drive %1").arg(i + 1));
    for (int i = 0; i < TAPE_NUM; i++)
        use(users, QString::fromUtf8(tape_drives[i].image_path), QCoreApplication::translate("SettingsDriveImages", "Tape drive %1").arg(i + 1));

    return users;
}

/* The parents of the differencing VHDs among files, and theirs: a parent
   is used by its child, whether or not a drive has the child. */
static void
addVhdParents(QHash<QString, QString> &users, const QStringList &files)
{
    QStringList   todo = files;
    QSet<QString> seen;

    while (!todo.isEmpty()) {
        const QString child = todo.takeFirst();
        char          parent[MAX_IMAGE_PATH_LEN];

        if (seen.contains(child) || !child.endsWith(QStringLiteral(".vhd"), Qt::CaseInsensitive))
            continue;
        seen.insert(child);
        if (!hdd_image_vhd_parent(child.toUtf8().constData(), parent, sizeof(parent)))
            continue;

        const QString key = imageKey(QString::fromUtf8(parent));
        if (key.isEmpty())
            continue;
        if (!users.contains(key))
            users.insert(key, QCoreApplication::translate("SettingsDriveImages", "Parent of %1").arg(QFileInfo(child).fileName()));
        todo.append(key);
    }
}

static Change *
changeFrom(const QString &from)
{
    for (auto &change : changes) {
        if (change.from == from)
            return &change;
    }

    return nullptr;
}

void
beginSession()
{
    changes.clear();
    usedAtStart = configuredUses();
}

bool
hasChanges()
{
    return !changes.isEmpty();
}

void
discard()
{
    changes.clear();
}

/* The renames and deletions, now that the Settings are saved; none of a
   file that the machine used when the dialog opened, that it uses now,
   or that is a parent of a differencing VHD. */
void
apply(QWidget *parent)
{
    if (changes.isEmpty())
        return;

    QHash<QString, QString> used = usedAtStart;
    const auto              now  = configuredUses();
    QStringList             files;

    for (auto it = now.cbegin(); it != now.cend(); ++it)
        used.insert(it.key(), it.value());
    files = used.keys();
    for (const auto &change : changes)
        files.append(change.from);
    QDirIterator it(QString::fromUtf8(usr_path), { QStringLiteral("*.vhd") }, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext())
        files.append(imageKey(it.next()));
    addVhdParents(used, files);

    QStringList problems;
    for (const auto &change : changes) {
        const QString name = QFileInfo(change.from).fileName();

        if (used.contains(change.from))
            problems.append(QCoreApplication::translate("SettingsDriveImages", "%1 is used (%2), and was left as it is.").arg(name, used.value(change.from)));
        else if (change.to.isEmpty()) {
            if (!QFile::remove(change.from))
                problems.append(QCoreApplication::translate("SettingsDriveImages", "%1 could not be deleted.").arg(name));
        } else if (QFileInfo::exists(change.to) || !QFile::rename(change.from, change.to))
            problems.append(QCoreApplication::translate("SettingsDriveImages", "%1 could not be renamed to %2.").arg(name, QFileInfo(change.to).fileName()));
    }
    changes.clear();

    if (!problems.isEmpty())
        QMessageBox::warning(parent, QCoreApplication::translate("SettingsDriveImages", "Disk Images"), problems.join('\n'));
}
}

SettingsDriveImages::SettingsDriveImages(SettingsHarddisks *harddisks, QWidget *parent)
    : QWidget(parent)
    , harddisks(harddisks)
{
    auto *layout  = new QVBoxLayout(this);
    auto *buttons = new QHBoxLayout();

    list = new QTreeWidget(this);
    list->setColumnCount(4);
    list->setHeaderLabels({ tr("Image"), tr("Kind"), tr("Size"), tr("Used by") });
    list->setRootIsDecorated(false);
    list->setSortingEnabled(true);
    list->sortByColumn(0, Qt::AscendingOrder);
    layout->addWidget(list);

    auto *note = new QLabel(tr("Images are renamed and deleted when the settings are saved with OK; Cancel leaves them as they are."), this);
    note->setWordWrap(true);
    layout->addWidget(note);

    auto *newDisk  = new QPushButton(tr("New hard disk..."), this);
    auto *existing = new QPushButton(tr("Add existing image..."), this);
    auto *rename   = new QPushButton(tr("Rename..."), this);
    auto *remove   = new QPushButton(tr("Delete"), this);
    auto *folder   = new QPushButton(tr("Open folder"), this);
    buttonRename   = rename;
    buttonDelete   = remove;
    for (auto *button : { newDisk, existing, rename, remove })
        buttons->addWidget(button);
    buttons->addStretch();
    buttons->addWidget(folder);
    layout->addLayout(buttons);

    auto click = [harddisks](const char *name) {
        if (auto *button = harddisks->findChild<QPushButton *>(name))
            button->click();
    };
    connect(newDisk, &QPushButton::clicked, this, [this, click]() {
        click("pushButtonNew");
        refresh();
    });
    connect(existing, &QPushButton::clicked, this, [this, click]() {
        click("pushButtonExisting");
        refresh();
    });
    connect(folder, &QPushButton::clicked, this, []() { QDesktopServices::openUrl(QUrl::fromLocalFile(QString(usr_path))); });
    connect(rename, &QPushButton::clicked, this, [this]() {
        const QString real  = selected();
        const QString shown = selectedShown();
        if (real.isEmpty())
            return;
        const QFileInfo info(shown);
        bool            ok   = false;
        const QString   name = QInputDialog::getText(this, tr("Rename Image"), tr("New name:"), QLineEdit::Normal, info.fileName(), &ok);
        if (!ok || name.isEmpty() || (name == info.fileName()))
            return;

        const QString target = info.dir().filePath(name);
        bool          taken  = QFileInfo::exists(target) && (DriveImages::imageKey(target) != real);
        for (const auto &change : DriveImages::changes)
            taken |= (change.to == target);
        if (taken) {
            QMessageBox::warning(this, tr("Rename Image"), tr("There is already an image named %1.").arg(name));
            return;
        }

        /* Renamed back to what it is: no change. */
        const bool back = (DriveImages::imageKey(target) == real);
        for (int i = 0; i < DriveImages::changes.size(); i++) {
            if (DriveImages::changes[i].from == real) {
                if (back)
                    DriveImages::changes.removeAt(i);
                else
                    DriveImages::changes[i].to = target;
                refresh();
                return;
            }
        }
        if (!back)
            DriveImages::changes.append({ real, target });
        refresh();
    });
    connect(remove, &QPushButton::clicked, this, [this]() {
        const QString real = selected();
        if (real.isEmpty())
            return;
        const QFileInfo info(selectedShown());
        QMessageBox     box(QMessageBox::Warning, tr("Delete Image"),
                            tr("Delete %1 when the settings are saved? Nothing uses it now, and once deleted it cannot be brought back.").arg(info.fileName()),
                            QMessageBox::Yes | QMessageBox::No, this);
        box.setDefaultButton(QMessageBox::No);
        if (box.exec() != QMessageBox::Yes)
            return;

        if (auto *change = DriveImages::changeFrom(real))
            change->to.clear();
        else
            DriveImages::changes.append({ real, QString() });
        refresh();
    });
    connect(list, &QTreeWidget::currentItemChanged, this, [this]() { selectionChanged(); });

    if (auto *view = harddisks->findChild<QTreeView *>("treeView")) {
        connect(view->model(), &QAbstractItemModel::rowsInserted, this, [this]() { refresh(); });
        connect(view->model(), &QAbstractItemModel::rowsRemoved, this, [this]() { refresh(); });
    }
}

void
SettingsDriveImages::showEvent(QShowEvent *event)
{
    refresh();
    QWidget::showEvent(event);
}

/* The selected image's file as it is now, when nothing uses it. */
QString
SettingsDriveImages::selected() const
{
    auto *item = list->currentItem();

    return ((item != nullptr) && item->data(3, Qt::UserRole).toBool() == false) ? item->data(0, Qt::UserRole).toString() : QString();
}

/* The selected image's path as it will be, after a pending rename. */
QString
SettingsDriveImages::selectedShown() const
{
    auto *item = list->currentItem();

    return (item != nullptr) ? item->data(0, Qt::UserRole + 1).toString() : QString();
}

/* Only an image nothing uses can be renamed or deleted. */
void
SettingsDriveImages::selectionChanged()
{
    const bool free = !selected().isEmpty();

    buttonRename->setEnabled(free);
    buttonDelete->setEnabled(free);
}

void
SettingsDriveImages::refresh()
{
    static const QStringList hardDisk  = { "hdi", "hdx", "vhd", "vhdx", "hdd" };
    static const QStringList floppy    = { "ima", "86f", "td0", "imd", "fdi", "flp", "vfd", "xdf", "dsk", "mfm" };
    static const QStringList cdImage   = { "iso", "cue", "mds", "mdf", "chd", "nrg" };
    static const QStringList removable = { "zdi", "mdi", "rdi" };

    /* Who uses what: the machine as it was when the dialog opened, the
       disks the Hard disks page holds now, the media in the other drives,
       and the parents of differencing VHDs. */
    QHash<QString, QString> users = DriveImages::usedAtStart;
    if (auto *view = harddisks->findChild<QTreeView *>("treeView")) {
        auto *model = view->model();
        for (int r = 0; r < model->rowCount(); r++)
            DriveImages::use(users, model->index(r, 1).data(Qt::UserRole).toString(), model->index(r, 0).data().toString());
    }
    /* Floppy drives as the Drives page names them: numbered on their
       controller. */
    const auto floppies = Cards::floppyControllers();
    for (int i = 0; i < FDD_NUM; i++) {
        int       number     = 0;
        const int controller = Cards::floppyControllerOf(floppies, i, &number);
        if (controller < 0)
            DriveImages::use(users, QString(drives[i].image_path), tr("Floppy drive %1").arg((i & 3) + 1));
        else if (controller == 0)
            DriveImages::use(users, QString(drives[i].image_path), tr("Floppy drive %1").arg(number));
        else
            DriveImages::use(users, QString(drives[i].image_path), tr("Floppy drive %1 (%2)").arg(number).arg(floppies[controller].name));
    }
    for (int i = 0; i < CDROM_NUM; i++)
        DriveImages::use(users, QString(cdrom[i].image_path), tr("CD-ROM drive %1").arg(i + 1));
    for (int i = 0; i < RDISK_NUM; i++)
        DriveImages::use(users, QString(rdisk_drives[i].image_path), tr("Removable disk drive %1").arg(i + 1));
    for (int i = 0; i < MO_NUM; i++)
        DriveImages::use(users, QString(mo_drives[i].image_path), tr("MO drive %1").arg(i + 1));
    for (int i = 0; i < TAPE_NUM; i++)
        DriveImages::use(users, QString(tape_drives[i].image_path), tr("Tape drive %1").arg(i + 1));

    QStringList  files;
    QDirIterator it(QString(usr_path), QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext())
        files.append(DriveImages::imageKey(it.next()));
    DriveImages::addVhdParents(users, users.keys() + files);

    const QString keep = selected();
    list->setSortingEnabled(false);
    list->clear();

    for (const QString &path : files) {
        const QFileInfo info(path);
        const QString   ext = info.suffix().toLower();

        QString kind;
        if (hardDisk.contains(ext))
            kind = tr("Hard disk");
        else if (floppy.contains(ext))
            kind = tr("Floppy disk");
        else if (cdImage.contains(ext))
            kind = tr("CD-ROM image");
        else if (removable.contains(ext))
            kind = tr("Removable disk");
        else if (ext == "img")
            /* A raw image: a floppy by its size, a hard disk otherwise. */
            kind = (info.size() <= (2880 * 1024)) ? tr("Floppy disk") : tr("Hard disk");
        else
            continue;

        /* As it will be once the Settings are saved: a file to be deleted
           is gone, one to be renamed has its new name. */
        const auto *change = DriveImages::changeFrom(path);
        if ((change != nullptr) && change->to.isEmpty())
            continue;
        const QString shown = (change != nullptr) ? change->to : path;

        const QString who  = users.value(path);
        auto         *item = new QTreeWidgetItem(list);
        item->setText(0, QDir(QString(usr_path)).relativeFilePath(shown));
        item->setData(0, Qt::UserRole, path);
        item->setData(0, Qt::UserRole + 1, shown);
        item->setText(1, kind);
        item->setText(2, QLocale().formattedDataSize(info.size()));
        item->setText(3, who.isEmpty() ? tr("(not used)") : who);
        item->setData(3, Qt::UserRole, !who.isEmpty());
        if (path == keep)
            list->setCurrentItem(item);
    }

    list->setSortingEnabled(true);
    for (int i = 0; i < list->columnCount(); i++)
        list->resizeColumnToContents(i);
    selectionChanged();
}
