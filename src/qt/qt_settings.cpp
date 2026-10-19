/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          Program settings UI module.
 *
 * Authors: Joakim L. Gilje <jgilje@jgilje.net>
 *          Cacodemon345
 *
 *          Copyright 2021 Joakim L. Gilje
 *          Copyright 2021-2022 Cacodemon345
 */
#include <cstdint>
#include <cstdio>

#include <QApplication>

#include "qt_defs.hpp"
#include "qt_settings.hpp"
#include "ui_qt_settings.h"
#include "qt_mainwindow.hpp"
#include "ui_qt_mainwindow.h"

extern "C" {
#include <86box/86box.h>
#include <86box/ini.h>
#include <86box/config.h>
#include <86box/keyboard.h>
#include <86box/plat.h>
#include <86box/ui.h>
#include <86box/device.h>
#include <86box/video.h>
#include <86box/timer.h>
#include <86box/fdd.h>
#include <86box/hdd.h>
#include <86box/hdc.h>
#include <86box/sound.h>
#include <86box/scsi.h>
#include <86box/scsi_device.h>
#include <86box/hdc_ide.h>
#include <86box/lpt.h>
#include <86box/serial.h>
#include <86box/midi.h>
#include <86box/machine.h>
}

#include <QStandardItemModel>
#include <QScrollBar>
#include <QScrollArea>
#include <QStyledItemDelegate>
#include <QPainter>
#include <QStyle>
#include <QVBoxLayout>
#include <QButtonGroup>
#include <QRadioButton>
#include <QAbstractItemView>
#include <QWheelEvent>
#include <QTimer>

#include "qt_settings_completer.hpp"

#include "qt_harddiskdialog.hpp"

#include "qt_settingsmachine.hpp"
#include "qt_settingsdisplay.hpp"
#include "qt_settingsinput.hpp"
#include "qt_settingssound.hpp"
#include "qt_settingsnetwork.hpp"
#include "qt_settingsports.hpp"
#include "qt_settingsstoragecontrollers.hpp"
#include "qt_settingsharddisks.hpp"
#include "qt_settingsfloppycdrom.hpp"
#include "qt_settingsotherremovable.hpp"
#include "qt_settingsotherperipherals.hpp"
#include "qt_settingsoverview.hpp"
#include "qt_settingsdrives.hpp"
#include "qt_deviceconfig.hpp"
#include "qt_settingscards.hpp"

#include "qt_preferences.hpp"

#include "qt_harddrive_common.hpp"
#include "qt_models_common.hpp"
#include "qt_settings_bus_tracking.hpp"

#include <QDebug>
#include <QComboBox>
#include <QMessageBox>
#include <QScreen>
#include <QTabWidget>
#include <QCheckBox>
#include <QStyle>

#include <dirent.h>
#include <unistd.h>

extern MainWindow *main_window;

class SettingsModel : public QAbstractListModel {
public:
    SettingsModel(QObject *parent)
        : QAbstractListModel(parent)
    {
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
        fontHeight = QFontMetrics(qApp->font()).height();
#else
        fontHeight = QApplication::fontMetrics().height();
#endif
    }

    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    int      rowCount(const QModelIndex &parent = QModelIndex()) const override;

private:
    QStringList pages = {
        "Machine",
        "Display",
        "Input devices",
        "Sound",
        "Network",
        "Ports (COM & LPT)",
        "Storage controllers",
        "Hard disks",
        "Floppy & CD-ROM drives",
        "Other removable devices",
        "Other peripherals",
    };
    QStringList page_icons = {
        "machine",
        "display",
        "input_devices",
        "sound",
        "network",
        "ports",
        "storage_controllers",
        "hard_disk",
        "floppy_and_cdrom_drives",
        "other_removable_devices",
        "other_peripherals",
    };
    int fontHeight;
};

QVariant
SettingsModel::data(const QModelIndex &index, int role) const
{
    Q_ASSERT(checkIndex(index, QAbstractItemModel::CheckIndexOption::IndexIsValid | QAbstractItemModel::CheckIndexOption::ParentIsInvalid));

    switch (role) {
        case Qt::DisplayRole:
            return tr(pages.at(index.row()).toUtf8().data());
        case Qt::DecorationRole:
            return QIcon(QString(":/settings/qt/icons/%1.ico").arg(page_icons[index.row()]));
        case Qt::SizeHintRole:
            return QSize(-1, fontHeight * 2);
        default:
            return {};
    }
}

int
SettingsModel::rowCount(const QModelIndex &parent) const
{
    (void) parent;
    return pages.size();
}

/* A page of the slot-based layout's list: its icon centred over its name,
   the width of the list. */
namespace {
class PageDelegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &) const override
    {
        const auto *view = qobject_cast<const QAbstractItemView *>(option.widget);
        const int   width = (view != nullptr) ? view->viewport()->width() : option.rect.width();

        return QSize(width, 32 + option.fontMetrics.height() + 18);
    }

    /* The style draws the row's background and its selection; the icon
       and name are drawn here, centred on the whole row. A style may move
       what it draws over for its own marks (Windows 11's selection bar),
       which would take them off centre. */
    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        QStyleOptionViewItem opt = option;
        initStyleOption(&opt, index);

        const QWidget *widget = option.widget;
        QStyle        *style  = (widget != nullptr) ? widget->style() : QApplication::style();
        const QString  text   = opt.text;

        opt.text.clear();
        opt.icon = QIcon();
        style->drawControl(QStyle::CE_ItemViewItem, &opt, painter, widget);

        const QRect row      = option.rect;
        const bool selected  = option.state & QStyle::State_Selected;
        const bool enabled   = option.state & QStyle::State_Enabled;
        QPalette rowPalette  = opt.palette;
        rowPalette.setCurrentColorGroup(enabled ? QPalette::Active : QPalette::Disabled);
        // Paint the selection and its text from the same palette pair. Native
        // Windows styles may use a pale selection, even behind a stylesheet.
        if (selected)
            painter->fillRect(row, rowPalette.brush(QPalette::Highlight));
        const QRect iconRect(row.x() + ((row.width() - 32) / 2), row.y() + 6, 32, 32);
        const QIcon icon     = qvariant_cast<QIcon>(index.data(Qt::DecorationRole));
        icon.paint(painter, iconRect, Qt::AlignCenter, enabled ? QIcon::Normal : QIcon::Disabled);

        const QPalette::ColorRole role = selected ? QPalette::HighlightedText : QPalette::Text;
        const QRect textRect(row.x() + 2, iconRect.bottom() + 4, row.width() - 4, row.bottom() - iconRect.bottom() - 4);
        style->drawItemText(painter, textRect, Qt::AlignHCenter | Qt::AlignTop | Qt::TextWordWrap, rowPalette, enabled, text, role);
    }

protected:
    void initStyleOption(QStyleOptionViewItem *option, const QModelIndex &index) const override
    {
        QStyledItemDelegate::initStyleOption(option, index);
        option->decorationPosition  = QStyleOptionViewItem::Top;
        option->decorationAlignment = Qt::AlignHCenter | Qt::AlignTop;
        option->displayAlignment    = Qt::AlignHCenter | Qt::AlignTop;
        option->decorationSize      = QSize(32, 32);
        option->features |= QStyleOptionViewItem::WrapText;
        option->textElideMode = Qt::ElideNone;
    }
};
}

Settings *Settings::settings = nullptr;
;
Settings::Settings(QWidget *parent)
    : QDialog(parent)
    , ui(new Ui::Settings)
    , slotLayout(settings_layout == 1)
{
    ui->setupUi(this);

    Settings::settings = this;
    configurationSnapshot = ini_clone(config_get_ini());

    Harddrives::busTrackClass = new SettingsBusTracking;

    /* Only the Machine page is built now. The others are built when first
       shown, or all at once on OK, so opening the dialog does not build
       every device list of every page. */
    machine            = new SettingsMachine(this);
    display            = nullptr;
    input              = nullptr;
    sound              = nullptr;
    network            = nullptr;
    ports              = nullptr;
    storageControllers = nullptr;
    harddisks          = nullptr;
    floppyCdrom        = nullptr;
    otherRemovable     = nullptr;
    otherPeripherals   = nullptr;

    ui->stackedWidget->addWidget(machine);
    for (int i = PAGE_DISPLAY; i < PAGE_COUNT; i++)
        ui->stackedWidget->addWidget(new QWidget(this));

    if (slotLayout) {
        setupSlotLayout();
        return;
    }

    auto *model = new SettingsModel(this);
    ui->listView->setModel(model);

    connect(ui->listView->selectionModel(), &QItemSelectionModel::currentChanged, this,
            [this](const QModelIndex &current, const QModelIndex &previous) {
                ensurePage(current.row());
                ui->stackedWidget->setCurrentIndex(current.row());
                ui->headerIcon->setPixmap(qvariant_cast<QIcon>(ui->listView->model()->data(current, Qt::DecorationRole)).pixmap(QSize(16, 16)));
                ui->headerLabel->setText(ui->listView->model()->data(current, Qt::DisplayRole).toString());
            });

    ui->listView->setCurrentIndex(model->index(0, 0));
}

/* The slot-based layout, a preview chosen in Preferences: the same pages,
   saved the same way, reached through Machine, Cards, Display, Input,
   Drives, Sound, Network and Ports down the left: Cards lists the cards, and the
   Display, Sound and Network pages show each card of theirs with its
   options; Machine is the whole machine as one tree. */
void
Settings::setupSlotLayout()
{
    auto *classicLink = new QLabel(this);
    classicLink->setObjectName(QStringLiteral("useClassicModeLink"));
    classicLink->setText(QStringLiteral("<a href=\"classic\">%1</a>").arg(tr("Use Classic Mode").toHtmlEscaped()));
    classicLink->setTextInteractionFlags(Qt::LinksAccessibleByMouse | Qt::LinksAccessibleByKeyboard);
    classicLink->setFocusPolicy(Qt::StrongFocus);
    classicLink->setToolTip(tr("Close this window and open Classic Settings."));
    ui->horizontalLayout_2->addWidget(classicLink);
    connect(classicLink, &QLabel::linkActivated, this, [this]() { switchToClassic(); });
    ensureAllPages();
    Cards::beginSession();

    panelHolder = new QWidget(this);
    panelHolder->hide();
    Cards::setPanelHolder(panelHolder);

    /* The classic pages' card rows are shown by the new pages; the rows go. */
    for (const char *name : { "comboBoxVideo", "comboBoxVideoSecondary", "checkBoxVoodoo", "checkBox8514", "checkBoxXga", "checkBoxDa2",
                              "comboBoxSoundCard1", "comboBoxSoundCard2", "comboBoxSoundCard3", "comboBoxSoundCard4",
                              "comboBoxNIC1", "comboBoxNIC2", "comboBoxNIC3", "comboBoxNIC4" })
        Borrow::hideRow(findChild<QWidget *>(name));
    if (auto *tabs = display->findChild<QTabWidget *>("tabWidgetDisplay"))
        tabs->removeTab(tabs->indexOf(display->findChild<QWidget *>("tabGeneral")));
    if (auto *tabs = findChild<QTabWidget *>("tabWidgetNet"))
        tabs->hide();

    /* Drives: every drive in one list, the selected one's settings
       beneath. */
    auto *drives = new SettingsDrives(harddisks, this);
    ui->stackedWidget->addWidget(drives);

    auto *cards        = new SettingsCards(this);
    auto *displayCards = new SettingsCardPage(Cards::CATEGORY_VIDEO, display, this);
    auto *soundCards   = new SettingsCardPage(Cards::CATEGORY_SOUND, sound, this);
    auto *networkCards = new SettingsCardPage(Cards::CATEGORY_NETWORK, network, this);
    for (auto *page : { static_cast<QWidget *>(cards), static_cast<QWidget *>(displayCards), static_cast<QWidget *>(soundCards),
                        static_cast<QWidget *>(networkCards) })
        ui->stackedWidget->addWidget(page);

    /* Machine is the machine as a tree; the Machine page itself opens from
       its rows' Options. */
    overview = new SettingsOverview({ machine, input, ports, harddisks, floppyCdrom }, this);
    auto *cardViews = new QWidget(this);
    auto *cardLayout = new QVBoxLayout(cardViews);
    cardLayout->setContentsMargins(0, 0, 0, 0);
    auto *modeRow = new QHBoxLayout();
    auto *basic = new QRadioButton(tr("Basic"), cardViews);
    auto *advanced = new QRadioButton(tr("Advanced"), cardViews);
    auto *modes = new QButtonGroup(cardViews);
    modes->addButton(basic, 0);
    modes->addButton(advanced, 1);
    modeRow->addWidget(basic);
    modeRow->addWidget(advanced);
    modeRow->addStretch();
    cardLayout->addLayout(modeRow);
    auto *description = new QLabel(cardViews);
    description->setWordWrap(true);
    cardLayout->addWidget(description);
    auto *views = new QStackedWidget(cardViews);
    ui->stackedWidget->removeWidget(cards);
    views->addWidget(cards);
    views->addWidget(overview);
    cardLayout->addWidget(views, 1);
    auto chooseView = [views, description](int mode) {
        views->setCurrentIndex(mode);
        description->setText(mode ? tr("Choose a board slot, then select the card to install. Onboard devices are grouped separately.")
                                  : tr("Add and configure cards. New cards use a free compatible slot; Advanced lets you choose the slot."));
        settings_advanced = mode;
    };
    connect(modes, &QButtonGroup::idClicked, this, chooseView);
    (settings_advanced ? advanced : basic)->setChecked(true);
    chooseView(settings_advanced ? 1 : 0);
    ui->stackedWidget->addWidget(cardViews);

    struct Entry {
        const char *title;
        const char *icon;
        QWidget    *page;
    };
    const Entry entries[] = {
        { QT_TR_NOOP("Machine"), "machine", machine },
        { QT_TR_NOOP("Cards"), "other_peripherals", cardViews },
        { QT_TR_NOOP("Display"), "display", displayCards },
        { QT_TR_NOOP("Input"), "input_devices", input },
        { QT_TR_NOOP("Drives"), "hard_disk", drives },
        { QT_TR_NOOP("Sound"), "sound", soundCards },
        { QT_TR_NOOP("Network"), "network", networkCards },
        { QT_TR_NOOP("Ports"), "ports", ports },
    };
    auto *model = new QStandardItemModel(this);
    for (const auto &entry : entries) {
        auto *item = new QStandardItem(QIcon(QString(":/settings/qt/icons/%1.ico").arg(entry.icon)), tr(entry.title));
        item->setData(QVariant::fromValue<QObject *>(entry.page), Qt::UserRole);
        item->setTextAlignment(Qt::AlignHCenter | Qt::AlignTop);
        item->setEditable(false);
        model->appendRow(item);
    }

    /* The pages down the left as large icons with their names beneath,
       each row the list's width with its icon and name centred in it. */
    const int columnWidth = 104;
    ui->listView->setViewMode(QListView::ListMode);
    ui->listView->setFlow(QListView::TopToBottom);
    ui->listView->setWrapping(false);
    ui->listView->setMovement(QListView::Static);
    ui->listView->setResizeMode(QListView::Adjust);
    ui->listView->setIconSize(QSize(32, 32));
    ui->listView->setWordWrap(true);
    ui->listView->setSpacing(0);
    ui->listView->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    ui->listView->setItemDelegate(new PageDelegate(ui->listView));
    ui->listView->setMinimumWidth(columnWidth);
    ui->listView->setMaximumWidth(columnWidth);
    ui->listView->viewport()->installEventFilter(this);

    QFont titleFont = ui->headerLabel->font();
    titleFont.setPointSize(titleFont.pointSize() + 2);
    titleFont.setBold(true);
    ui->headerLabel->setFont(titleFont);
    ui->headerLabel->setContentsMargins(4, 3, 0, 6);
    // Native controls and system colours retain the classic desktop appearance.
    setStyleSheet("QTreeWidget { alternate-background-color: palette(alternate-base); } "
                  "QTreeWidget::item { min-height: 25px; } "
                  "QTreeWidget::item:hover { background: #edf4fa; color: #182633; } "
                  "QTreeWidget::item:selected { background: #dceaf5; color: #182633; } "
                  "QTreeWidget::item:selected:hover { background: #d3e4f2; color: #182633; } "
                  "QGroupBox { margin-top: 10px; padding-top: 10px; } "
                  "QGroupBox::title { subcontrol-origin: margin; left: 8px; padding: 0 4px; }");
    qApp->installEventFilter(this);
    setMinimumSize(780, 580);
    setMaximumSize(QWIDGETSIZE_MAX, QWIDGETSIZE_MAX);
    setSizeGripEnabled(true);
    const QRect screen = this->screen()->availableGeometry();
    resize(qMin(1024, screen.width() - 40), qMin(800, screen.height() - 80));

    ui->listView->setModel(model);
    connect(ui->listView->selectionModel(), &QItemSelectionModel::currentChanged, this,
            [this](const QModelIndex &current, const QModelIndex &) { showListPage(current); });
    ui->listView->setCurrentIndex(model->index(0, 0));
}

bool
Settings::hasPendingEdits()
{
    const int parts[] = { machine->changed(), display->changed(), input->changed(), sound->changed(), network->changed(),
                          ports->changed(), storageControllers->changed(), harddisks->changed(), floppyCdrom->changed(),
                          otherRemovable->changed(), otherPeripherals->changed() };
    int changed = 0;
    for (const int part : parts)
        changed |= part;
    const bool slots_changed = Cards::slotsChanged();
    const bool ini_changed   = !ini_equal(config_get_ini(), configurationSnapshot);
    bool       panel_changed = false;
    for (auto *panel : DeviceConfigPanel::panels())
        panel_changed |= panel->hasPendingEdits();
    return changed || slots_changed || ini_changed || panel_changed;
}

void
Settings::switchToClassic()
{
    if (hasPendingEdits()) {
        QMessageBox question(QMessageBox::Question, tr("Use Classic Mode"),
            tr("You have unsaved changes. Close without saving and open Classic Settings?"),
            QMessageBox::Discard | QMessageBox::Cancel, this);
        question.button(QMessageBox::Discard)->setText(tr("Close without saving"));
        question.setDefaultButton(QMessageBox::Cancel);
        if (question.exec() != QMessageBox::Discard)
            return;
    }
    settings_layout = 0;
    config_save_global();
    ini_restore(config_get_ini(), configurationSnapshot);
    done(ReopenClassic);
}

void
Settings::showListPage(const QModelIndex &current)
{
    auto *page = qobject_cast<QWidget *>(current.data(Qt::UserRole).value<QObject *>());
    if (page == nullptr)
        return;

    ui->stackedWidget->setCurrentWidget(page);
    ui->headerIcon->setPixmap(qvariant_cast<QIcon>(current.data(Qt::DecorationRole)).pixmap(QSize(16, 16)));
    ui->headerLabel->setText(current.data(Qt::DisplayRole).toString());
}

/* A page takes the place of its placeholder in the stack. */
void
Settings::placePage(int index, QWidget *page)
{
    QWidget *placeholder = ui->stackedWidget->widget(index);

    /* The Storage page's twelve controller rows scroll; the slot-based
       layout has its own scrolling. */
    QWidget *shown = page;
    if (!slotLayout && (index == PAGE_STORAGE)) {
        auto *scroll = new QScrollArea(this);
        scroll->setFrameShape(QFrame::NoFrame);
        scroll->setWidgetResizable(true);
        scroll->setWidget(page);
        shown = scroll;
    }
    ui->stackedWidget->insertWidget(index, shown);
    ui->stackedWidget->removeWidget(placeholder);
    delete placeholder;
}

/* Build the page at a list index if it is not built yet, with the pages it
   depends on: Display before Input (Input asks it about the light pen), and
   Ports before the three drive pages, which are built together: they share
   the bus channel tracking, where Ports marks the parallel ports in use. A
   page built after the machine was changed is brought up to that machine. */
void
Settings::ensurePage(int index)
{
    const int machineId = machine->currentMachineId();

    switch (index) {
        case PAGE_DISPLAY:
        case PAGE_INPUT:
            if (display == nullptr) {
                display = new SettingsDisplay(this);
                placePage(PAGE_DISPLAY, display);
                connect(machine, &SettingsMachine::currentMachineChanged, display,
                        &SettingsDisplay::onCurrentMachineChanged);
            }
            if (input == nullptr) {
                input = new SettingsInput(this);
                placePage(PAGE_INPUT, input);
                connect(machine, &SettingsMachine::currentMachineChanged, input,
                        &SettingsInput::onCurrentMachineChanged);
                if (machineId != ::machine)
                    display->onCurrentMachineChanged(machineId); /* and Input with it */
            }
            break;

        case PAGE_SOUND:
            if (sound == nullptr) {
                sound = new SettingsSound(this);
                placePage(PAGE_SOUND, sound);
                connect(machine, &SettingsMachine::currentMachineChanged, sound,
                        &SettingsSound::onCurrentMachineChanged);
                if (machineId != ::machine)
                    sound->onCurrentMachineChanged(machineId);
            }
            break;

        case PAGE_NETWORK:
            if (network == nullptr) {
                network = new SettingsNetwork(this);
                placePage(PAGE_NETWORK, network);
                connect(machine, &SettingsMachine::currentMachineChanged, network,
                        &SettingsNetwork::onCurrentMachineChanged);
                if (machineId != ::machine)
                    network->onCurrentMachineChanged(machineId);
            }
            break;

        case PAGE_PORTS:
            if (ports == nullptr) {
                ports = new SettingsPorts(this);
                placePage(PAGE_PORTS, ports);
                connect(machine, &SettingsMachine::currentMachineChanged, ports,
                        &SettingsPorts::onCurrentMachineChanged);
                if (machineId != ::machine)
                    ports->onCurrentMachineChanged(machineId);
            }
            break;

        case PAGE_STORAGE:
            if (storageControllers == nullptr) {
                storageControllers = new SettingsStorageControllers(this);
                placePage(PAGE_STORAGE, storageControllers);
                connect(machine, &SettingsMachine::currentMachineChanged, storageControllers,
                        &SettingsStorageControllers::onCurrentMachineChanged);
                if (machineId != ::machine)
                    storageControllers->onCurrentMachineChanged(machineId);
            }
            break;

        case PAGE_HARDDISKS:
        case PAGE_FLOPPYCDROM:
        case PAGE_REMOVABLE:
            ensurePage(PAGE_PORTS);
            if (harddisks == nullptr) {
                harddisks      = new SettingsHarddisks(this);
                floppyCdrom    = new SettingsFloppyCDROM(this);
                otherRemovable = new SettingsOtherRemovable(this);
                placePage(PAGE_HARDDISKS, harddisks);
                placePage(PAGE_FLOPPYCDROM, floppyCdrom);
                placePage(PAGE_REMOVABLE, otherRemovable);

                connect(machine, &SettingsMachine::currentMachineChanged, floppyCdrom,
                        &SettingsFloppyCDROM::onCurrentMachineChanged);
                connect(floppyCdrom, &SettingsFloppyCDROM::cdromChannelChanged, harddisks,
                        &SettingsHarddisks::reloadBusChannels);
                connect(floppyCdrom, &SettingsFloppyCDROM::cdromChannelChanged, otherRemovable,
                        &SettingsOtherRemovable::reloadBusChannels_MO);
                connect(floppyCdrom, &SettingsFloppyCDROM::cdromChannelChanged, otherRemovable,
                        &SettingsOtherRemovable::reloadBusChannels_RDisk);
                connect(harddisks, &SettingsHarddisks::driveChannelChanged, floppyCdrom,
                        &SettingsFloppyCDROM::reloadBusChannels);
                connect(harddisks, &SettingsHarddisks::driveChannelChanged, otherRemovable,
                        &SettingsOtherRemovable::reloadBusChannels_MO);
                connect(harddisks, &SettingsHarddisks::driveChannelChanged, otherRemovable,
                        &SettingsOtherRemovable::reloadBusChannels_RDisk);
                connect(otherRemovable, &SettingsOtherRemovable::moChannelChanged, harddisks,
                        &SettingsHarddisks::reloadBusChannels);
                connect(otherRemovable, &SettingsOtherRemovable::moChannelChanged, floppyCdrom,
                        &SettingsFloppyCDROM::reloadBusChannels);
                connect(otherRemovable, &SettingsOtherRemovable::moChannelChanged, otherRemovable,
                        &SettingsOtherRemovable::reloadBusChannels_RDisk);
                connect(otherRemovable, &SettingsOtherRemovable::rdiskChannelChanged, harddisks,
                        &SettingsHarddisks::reloadBusChannels);
                connect(otherRemovable, &SettingsOtherRemovable::rdiskChannelChanged, floppyCdrom,
                        &SettingsFloppyCDROM::reloadBusChannels);
                connect(otherRemovable, &SettingsOtherRemovable::rdiskChannelChanged, otherRemovable,
                        &SettingsOtherRemovable::reloadBusChannels_MO);
                connect(harddisks, &SettingsHarddisks::driveChannelChanged, otherRemovable,
                        &SettingsOtherRemovable::reloadBusChannels_Tape);
                connect(otherRemovable, &SettingsOtherRemovable::tapeChannelChanged, harddisks,
                        &SettingsHarddisks::reloadBusChannels);
                connect(otherRemovable, &SettingsOtherRemovable::tapeChannelChanged, floppyCdrom,
                        &SettingsFloppyCDROM::reloadBusChannels);
                connect(otherRemovable, &SettingsOtherRemovable::tapeChannelChanged, otherRemovable,
                        &SettingsOtherRemovable::reloadBusChannels_MO);
                connect(otherRemovable, &SettingsOtherRemovable::tapeChannelChanged, otherRemovable,
                        &SettingsOtherRemovable::reloadBusChannels_RDisk);
                connect(otherRemovable, &SettingsOtherRemovable::moChannelChanged, otherRemovable,
                        &SettingsOtherRemovable::reloadBusChannels_Tape);
                connect(otherRemovable, &SettingsOtherRemovable::rdiskChannelChanged, otherRemovable,
                        &SettingsOtherRemovable::reloadBusChannels_Tape);

                if (machineId != ::machine)
                    floppyCdrom->onCurrentMachineChanged(machineId);
            }
            break;

        case PAGE_OTHER:
            if (otherPeripherals == nullptr) {
                otherPeripherals = new SettingsOtherPeripherals(this);
                placePage(PAGE_OTHER, otherPeripherals);
                connect(machine, &SettingsMachine::currentMachineChanged, otherPeripherals,
                        &SettingsOtherPeripherals::onCurrentMachineChanged);
                if (machineId != ::machine)
                    otherPeripherals->onCurrentMachineChanged(machineId);
            }
            break;

        default:
            break;
    }
}

/* OK builds every page not built yet before anything is checked or saved,
   as when all were built on opening: each page sees the selected machine
   and drops from its selection what that machine cannot take, and whatever
   else it corrects in the saved configuration. */
void
Settings::ensureAllPages()
{
    for (int i = PAGE_DISPLAY; i < PAGE_COUNT; i++)
        ensurePage(i);
}

Settings::~Settings()
{
    qApp->removeEventFilter(this);
    if (slotLayout)
        config_save_global();
    /* What the slot-based layout borrowed goes back before anything goes. */
    Borrow::giveBackAll();
    delete ui;
    ini_close(configurationSnapshot);
    /* The device lists are read again the next time: ROMs may come or go
       in between. */
    Models::ClearDevices();
    delete Harddrives::busTrackClass;
    Harddrives::busTrackClass = nullptr;
    Settings::settings        = nullptr;
}

int
Settings::currentMachine() const
{
    return machine->currentMachineId();
}

int
Settings::currentHdc(int i) const
{
    return (storageControllers != nullptr) ? storageControllers->hdcCard(i) : hdc_current[i];
}

int
Settings::currentSoundCard(int i) const
{
    return (sound != nullptr) ? sound->soundCard(i) : sound_card_current[i];
}

int
Settings::currentScsiCard(int i) const
{
    return (storageControllers != nullptr) ? storageControllers->scsiCard(i) : scsi_card_current[i];
}

/* [Board slots] is the slot-based layout's (Cards::saveSlots()), and the
   classic dialog shows none of it: whether the section still places this
   machine's cards. */
static bool
boardSlotsPlace(int mach)
{
    const char *board = config_get_string(const_cast<char *>("Board slots"), const_cast<char *>("machine"), const_cast<char *>(""));

    return (board[0] != '\0') && !strcmp(board, machines[mach].internal_name);
}

void
Settings::save(int soft)
{
    ensureAllPages();

    machine->save(soft);
    display->save(soft);
    input->save(soft);
    sound->save(soft);
    network->save(soft);
    ports->save(soft);
    storageControllers->save(soft);
    harddisks->save(soft);
    floppyCdrom->save(soft);
    otherRemovable->save(soft);
    otherPeripherals->save(soft);
    if (slotLayout)
        Cards::saveSlots();
    else if (auto *section = config_find_section(const_cast<char *>("Board slots"))) {
        /* Saved from the classic dialog, the machine is what it shows: its
           cards where 86Box puts them, an EISA card in the slot its own
           option names (which the slot-based layout keeps the same), and
           no PCI-PCI bridge put in by hand. */
        ini_section_clear(section);
        ini_delete_section_if_empty(config_get_ini(), section);
    }
}

void
Settings::accept()
{
    int changed = 0;

    ensureAllPages();
    /* A controller that cannot have the IDE channels it needs (two cards
       that can only use the legacy ports, say) will not work: ask. */
    ide_owner_t    owners[IDE_BUS_MAX];
    ide_conflict_t conflicts[IDE_CONFLICTS_MAX];
    int            conflictCount = 0;
    const int      owned         = Harddrives::idePlan(owners, conflicts, &conflictCount);

    if (conflictCount > 0) {
        QStringList lines;
        for (int i = 0; i < conflictCount; i++)
            lines.append(Harddrives::conflictText(i, owners, owned, conflicts, conflictCount));

        QMessageBox box(QMessageBox::Icon::Warning, tr("IDE Conflict"),
                        tr("These IDE controllers need channels that another device already has, and will not work:") +
                            QString("\n\n%1\n\n").arg(lines.join("\n")) +
                            tr("Do you want to save the configuration anyway?"),
                        QMessageBox::Yes | QMessageBox::No, this);
        box.setDefaultButton(QMessageBox::No);
        if (box.exec() != QMessageBox::Yes)
            return;
    }

    if (slotLayout) {
        const QString error = Cards::slotError();
        if (!error.isEmpty()) {
            QMessageBox::warning(this, tr("Board slot conflict"), error);
            return;
        }
        if (Cards::slotsChanged())
            changed |= SETTINGS_CHANGED | SETTINGS_REQUIRE_HARD_RESET;
    }

    /* The classic dialog drops [Board slots] (save()): for the machine it
       placed cards on, that moves them. */
    if (!slotLayout && boardSlotsPlace(currentMachine()))
        changed |= SETTINGS_CHANGED | SETTINGS_REQUIRE_HARD_RESET;

    /* The options shown inline, written as the Configure dialogs write them. */
    if (DeviceConfigPanel::applyAll(nullptr))
        changed |= SETTINGS_CHANGED | SETTINGS_REQUIRE_HARD_RESET;

    changed |= machine->changed();
    changed |= display->changed();
    changed |= input->changed();
    changed |= sound->changed();
    changed |= network->changed();
    changed |= ports->changed();
    changed |= storageControllers->changed();
    changed |= harddisks->changed();
    changed |= floppyCdrom->changed();
    changed |= otherRemovable->changed();
    changed |= otherPeripherals->changed();

    if ((changed & SETTINGS_REQUIRE_HARD_RESET) && confirm_save && !settings_only) {
        QMessageBox questionbox(QMessageBox::Icon::Question, "86Box",
                                tr("Do you want to save the settings?"),
                                QMessageBox::Save | QMessageBox::Cancel, this);
        QCheckBox  *chkbox = new QCheckBox(tr("Don't show this message again"));
        questionbox.setCheckBox(chkbox);
        questionbox.setInformativeText(tr("This will hard reset the emulated machine."));
        chkbox->setChecked(!confirm_save);
        QObject::connect(chkbox, &QCheckBox::CHECK_STATE_CHANGED, [](int state) { confirm_save = (state == Qt::CheckState::Unchecked); });
        questionbox.exec();
        if (questionbox.result() == QMessageBox::Cancel) {
            confirm_save = true;
            return;
        }
    } else if (changed && !(changed & SETTINGS_REQUIRE_HARD_RESET) && !settings_only) {
        save(1);
        config_changed = 2;
        main_window->emitVmmSignal();
        lpt_devices_reset();
        serial_devices_reset();
        midi_config_changed();

        video_copy = (video_grayscale || invert_display) ? video_transform_copy : memcpy;
        config_save();
        reset_screen_size();
        device_force_redraw();
        for (int i = 0; i < MONITORS_NUM; i++) {
            if (monitors[i].target_buffer)
                video_force_resize_set_monitor(1, i);
        }

        /* Reject so the main window does nothing. */
        QDialog::reject();
        return;
    } else if (!changed && !settings_only) {
        QDialog::reject();
        return;
    }

    QDialog::accept();
}

static int
plat_path_is_empty(char *path)
{
    int n            = 0;
    DIR *dir         = opendir(path);
    struct dirent *d;

    if (dir == NULL)
        /* Not a directory or doesn't exist. */
        return 1;

    while ((d = readdir(dir)) != NULL) {
        if (++n > 2)
            break;
    }

    closedir(dir);

    return (n <= 2);
}

void
Settings::reject()
{
    /* In the slot-based layout, Cancel leaves the configuration as the
       dialog found it, with what its Configure dialogs wrote undone. The
       classic dialog leaves those, as it always has. */
    if (slotLayout)
        ini_restore(config_get_ini(), configurationSnapshot);

    if (plat_path_is_empty(usr_path))
        rmdir(usr_path);

    QDialog::reject();
}

/* The page list's cells: the width that shows of the list, and the height
   of the list, laid out again when the list's width changes. */
void
Settings::fitPageList()
{
    ui->listView->doItemsLayout();
}

bool
Settings::eventFilter(QObject *watched, QEvent *event)
{
    // Scrolling Settings must never edit the control beneath the pointer.
    if (event->type() == QEvent::Wheel) {
        auto *widget = qobject_cast<QWidget *>(watched);
        auto *combo = qobject_cast<QComboBox *>(widget);
        if (!combo && widget)
            combo = qobject_cast<QComboBox *>(widget->parentWidget());
        if (combo && isAncestorOf(combo) && !combo->view()->isVisible()) {
            QWidget *parent = combo->parentWidget();
            while (parent && parent != this) {
                if (auto *scroll = qobject_cast<QAbstractScrollArea *>(parent)) {
                    QCoreApplication::sendEvent(scroll->viewport(), event);
                    return true;
                }
                parent = parent->parentWidget();
            }
            return true;
        }
    }
    if ((watched == ui->listView->viewport()) && (event->type() == QEvent::Resize))
        fitPageList();

    return QDialog::eventFilter(watched, event);
}
