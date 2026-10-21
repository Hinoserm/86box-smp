/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          The cards of the slot-based Settings layout (preview): what the
 *          settings hold, where 86Box puts it, and the pages that show it.
 */
#include <algorithm>
#include <functional>

#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHash>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeyEvent>
#include <QLabel>
#include <QMap>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSet>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QTabWidget>
#include <QToolButton>
#include <QTreeWidget>
#include <QLineEdit>
#include <QScreen>
#include <QTimer>
#include <QTreeWidgetItemIterator>
#include <QVector>
#include <QFrame>
#include <QDialog>
#include <QDialogButtonBox>
#include <QVBoxLayout>

extern "C" {
#include <86box/86box.h>
#include <86box/timer.h>
#include <86box/thread.h>
#include <86box/device.h>
#include <86box/ini.h>
#include <86box/config.h>
#include <86box/machine.h>
#include <86box/video.h>
#include <86box/vid_8514a_device.h>
#include <86box/vid_xga_device.h>
#include <86box/vid_ps55da2.h>
#include <86box/sound.h>
#include <86box/network.h>
#include <86box/hdc.h>
#include <86box/scsi.h>
#include <86box/fdc_ext.h>
#include <86box/cdrom_interface.h>
#include <86box/isamem.h>
#include <86box/mcamem.h>
#include <86box/isarom.h>
#include <86box/isartc.h>
#include <86box/ibm_5161.h>
#include <86box/lpt.h>
#include <86box/mouse.h>
#include <86box/serial.h>
#include <86box/gameport.h>
#include <86box/bugger.h>
#include <86box/postcard.h>
#include <86box/unittester.h>
#include <86box/novell_cardkey.h>
#include <86box/softpower.h>
#include <86box/pci.h>
}

#include "qt_deviceconfig.hpp"
#include "qt_settings.hpp"
#include "qt_settingscards.hpp"

namespace Cards {

static QHash<QString, int> assignments;
static QList<int> bridges; /* the board slots of the PCI-PCI bridges put in by hand */

/* The views that follow the cards (followCards()): told when the layout
   changes with no card control changing, as when a bridge goes in. */
static QList<QPair<QPointer<QWidget>, std::function<void()>>> followers;

void
follow(QWidget *view, const std::function<void()> &changed)
{
    followers.erase(std::remove_if(followers.begin(), followers.end(), [](const auto &follower) { return follower.first.isNull(); }),
                    followers.end());
    followers.append({ QPointer<QWidget>(view), changed });
}

static void
layoutChanged()
{
    for (const auto &follower : followers) {
        if (!follower.first.isNull())
            follower.second();
    }
}
static int assignmentMachine = -1;
static bool assignmentsEdited = false;

static QString deviceKey(const _device_ *dev, int instance)
{
    return dev ? QString("%1.%2").arg(dev->internal_name).arg(instance) : QString();
}

void beginSession()
{
    assignments.clear();
    bridges.clear();
    assignmentMachine = Settings::settings->currentMachine();
    assignmentsEdited = false;
    if (!strcmp(config_get_string("Board slots", "machine", const_cast<char *>("")), machines[assignmentMachine].internal_name)) {
        for (int n = 1;; n++) {
            const int slot = config_get_int("Board slots", QString("pci_bridge.%1").arg(n).toUtf8().data(), -1);
            if (slot < 0)
                break;
            bridges.append(slot);
        }
    }
    for (const auto &card : list()) {
        if (!card.dev || card.onboard)
            continue;
        int value = device_board_slot(card.dev, card.instance, assignmentMachine);
        if ((value < 0) && (card.dev->flags & DEVICE_EISA)) {
            device_context_t context;
            device_set_context(&context, card.dev, card.instance);
            for (auto *c = card.dev->config; c && c->type != CONFIG_END; ++c) {
                if (!strcmp(c->name, "slot"))
                    value = (SLOT_EISA << 16) | config_get_int(context.name, "slot", c->default_int);
            }
        }
        if (value >= 0)
            assignments.insert(deviceKey(card.dev, card.instance), value);
    }
}

/* Placements and bridges are a machine's: another machine chosen starts
   with none. */
static void
followMachine()
{
    if (Settings::settings && assignmentMachine != Settings::settings->currentMachine()) {
        assignments.clear();
        bridges.clear();
        assignmentMachine = Settings::settings->currentMachine();
        assignmentsEdited = true;
    }
}

int placement(const _device_ *dev, int instance)
{
    followMachine();
    return assignments.value(deviceKey(dev, instance), -1);
}

void assign(const Card &card, int slot)
{
    if (!card.dev || card.onboard)
        return;
    const auto key = deviceKey(card.dev, card.instance);
    if (assignments.value(key, -1) != slot) {
        assignments.insert(key, slot);
        assignmentsEdited = true;
    }
    for (auto *panel : DeviceConfigPanel::panels()) {
        if ((panel->device() == card.dev) && (panel->instance() == card.instance)) {
            if (auto *box = panel->findChild<QComboBox *>("slot"); box && (slot >> 16) == SLOT_EISA) {
                box->setCurrentIndex(box->findData(slot & 0xffff));
                box->setEnabled(false);
                box->setToolTip(QCoreApplication::translate("SettingsCards", "Choose the board slot on the Cards page."));
            }
        }
    }
}

bool slotsChanged()
{
    return assignmentsEdited;
}

QString slotError()
{
    const auto cards = list();
    const auto nodes = layout(cards);
    QSet<int> seen;
    for (int i = 0; i < cards.size(); ++i) {
        if (cards[i].onboard || !cards[i].dev)
            continue;
        const int slot = placement(cards[i].dev, cards[i].instance);
        if (slot < 0)
            continue;
        bool found = false;
        for (const auto &node : nodes)
            found |= (node.slot == slot) && (node.card == i);
        if (!found || seen.contains(slot))
            return QCoreApplication::translate("SettingsCards", "%1 has an unavailable board slot. Choose a free compatible slot on the Cards page.").arg(cards[i].name);
        seen.insert(slot);
    }
    return QString();
}

void saveSlots()
{
    if (!Settings::settings)
        return;
    const auto cards = list();
    const auto nodes = layout(cards);
    ini_section_clear(config_find_section("Board slots"));
    config_set_string("Board slots", "machine", machines[Settings::settings->currentMachine()].internal_name);
    /* The bridges in their order: it numbers the buses behind them. */
    QMap<int, int> made;
    for (const auto &node : nodes) {
        if (node.bridge > 0)
            made.insert(node.bridge, node.slot);
    }
    int n = 0;
    for (const int slot : made)
        config_set_int("Board slots", QString("pci_bridge.%1").arg(++n).toUtf8().data(), slot);
    for (const auto &node : nodes) {
        if (node.card < 0 || node.slot < 0)
            continue;
        const auto &card = cards[node.card];
        if (!card.dev || card.onboard)
            continue;
        config_set_int("Board slots", deviceKey(card.dev, card.instance).toUtf8().constData(), node.slot);
        if ((node.slot >> 16) == SLOT_EISA && (card.dev->flags & DEVICE_EISA)) {
            device_context_t context;
            device_set_context(&context, card.dev, card.instance);
            config_set_int(context.name, "slot", node.slot & 0xffff);
        }
    }
}


/* Which of the machine's own devices a category's "internal" entry is. */
enum Onboard {
    ONBOARD_NONE = 0,
    ONBOARD_VIDEO,
    ONBOARD_SOUND,
    ONBOARD_NETWORK,
    ONBOARD_HDC,
    ONBOARD_FDC
};

/* A card that is there or not, by a check box. */
struct CheckCard {
    const char *control;
    const char *button;
    const device_t *(*device)(int mach);
};

/* A kind of card and the controls on the classic pages that hold it: one
   combo box per card the machine can have, with its Configure button, and
   the check boxes of the cards that are there or not. */
struct CardCategory {
    const char      *title;
    QStringList      combos;
    QStringList      buttons;
    const device_t *(*device)(int id);
    int              onboard;
    QList<CheckCard> checks;
    bool             automatic = false; /* entry 1 is "automatic", not a card */
};

/* A serial or parallel port past the board's own is a port card, an MCA
   one on a Micro Channel board; the board's own are not cards. */
template <int N>
static const device_t *
serialCard(int mach)
{
    if (N <= machine_get_serial_ports(mach))
        return nullptr;
    return machine_has_bus(mach, MACHINE_BUS_MCA) ? &serial_card_mca_device : &serial_card_device;
}

template <int N>
static const device_t *
parallelCard(int mach)
{
    if (N <= machine_get_parallel_ports(mach))
        return nullptr;
    return machine_has_bus(mach, MACHINE_BUS_MCA) ? &lpt_card_mca_device : &lpt_card_device;
}

static bool
isSerialCard(const device_t *dev)
{
    return (dev == &serial_card_device) || (dev == &serial_card_mca_device);
}

static bool
isPortCard(const device_t *dev)
{
    return isSerialCard(dev) || (dev == &lpt_card_device) || (dev == &lpt_card_mca_device);
}

/* The IBM 5161 of IBM's machines that take it is their own option
   (enable_5161), not a card of the Expansion unit list: the unit when the
   machine's option has it. */
static const device_t *
machineExpansionUnit(int mach)
{
    const device_t *dev = machine_get_device(mach);

    if ((dev == nullptr) || (dev->config == nullptr))
        return nullptr;

    for (const device_config_t *c = dev->config; c->type != CONFIG_END; c++) {
        if (!strcmp(c->name, "enable_5161")) {
            device_context_t ctx;

            device_set_context(&ctx, dev, 0);
            return config_get_int(ctx.name, const_cast<char *>("enable_5161"), c->default_int) ? &ibm_5161_device : nullptr;
        }
    }

    return nullptr;
}

static const device_t *
memory_card(int id)
{
    return machine_has_bus(Settings::settings->currentMachine(), MACHINE_BUS_MCA) ? mcamem_get_device(id) : isamem_get_device(id);
}

static QStringList
numberedControls(const char *prefix, int count)
{
    QStringList names;
    for (int i = 1; i <= count; ++i)
        names.append(QString::fromLatin1(prefix) + QString::number(i));
    return names;
}

static const QList<CardCategory> &
categories()
{
    static const QList<CardCategory> list = {
        { QT_TRANSLATE_NOOP("SettingsCards", "Video"), { "comboBoxVideo", "comboBoxVideoSecondary" },
          { "pushButtonConfigureVideo", "pushButtonConfigureVideoSecondary" }, video_card_getdevice, ONBOARD_VIDEO,
          { { "checkBoxVoodoo", "pushButtonConfigureVoodoo", [](int) -> const device_t * { return &voodoo_device; } },
            { "checkBox8514", "pushButtonConfigure8514",
              [](int mach) -> const device_t * { return machine_has_bus(mach, MACHINE_BUS_MCA) ? &ibm8514_mca_device : &gen8514_isa_device; } },
            { "checkBoxXga", "pushButtonConfigureXga", [](int) -> const device_t * { return &xga_device; } },
            { "checkBoxDa2", "pushButtonConfigureDa2", [](int) -> const device_t * { return &ps55da2_device; } } } },
        { QT_TRANSLATE_NOOP("SettingsCards", "Sound"), { "comboBoxSoundCard1", "comboBoxSoundCard2", "comboBoxSoundCard3", "comboBoxSoundCard4" },
          { "pushButtonConfigureSoundCard1", "pushButtonConfigureSoundCard2", "pushButtonConfigureSoundCard3", "pushButtonConfigureSoundCard4" },
          sound_card_getdevice, ONBOARD_SOUND, {} },
        { QT_TRANSLATE_NOOP("SettingsCards", "Network"), { "comboBoxNIC1", "comboBoxNIC2", "comboBoxNIC3", "comboBoxNIC4" },
          { "pushButtonConf1", "pushButtonConf2", "pushButtonConf3", "pushButtonConf4" }, network_card_getdevice, ONBOARD_NETWORK, {} },
        { QT_TRANSLATE_NOOP("SettingsCards", "Disk controller"), numberedControls("comboBoxHD", HDC_MAX),
          numberedControls("pushButtonHD", HDC_MAX), hdc_get_device, ONBOARD_HDC, {} },
        { QT_TRANSLATE_NOOP("SettingsCards", "SCSI"), { "comboBoxSCSI1", "comboBoxSCSI2", "comboBoxSCSI3", "comboBoxSCSI4" },
          { "pushButtonSCSI1", "pushButtonSCSI2", "pushButtonSCSI3", "pushButtonSCSI4" }, scsi_card_getdevice, ONBOARD_NONE, {} },
        { QT_TRANSLATE_NOOP("SettingsCards", "Floppy controller"), { "comboBoxFD" }, { "pushButtonFD" }, fdc_card_getdevice, ONBOARD_FDC, {} },
        { QT_TRANSLATE_NOOP("SettingsCards", "CD-ROM interface"), { "comboBoxCDInterface" }, { "pushButtonCDInterface" },
          cdrom_interface_get_device, ONBOARD_NONE, {} },
        { QT_TRANSLATE_NOOP("SettingsCards", "Memory expansion"), { "comboBoxMemExpCard1", "comboBoxMemExpCard2", "comboBoxMemExpCard3", "comboBoxMemExpCard4" },
          { "pushButtonConfigureMemExpCard1", "pushButtonConfigureMemExpCard2", "pushButtonConfigureMemExpCard3", "pushButtonConfigureMemExpCard4" },
          memory_card, ONBOARD_NONE, {} },
        { QT_TRANSLATE_NOOP("SettingsCards", "ROM expansion"), { "comboBoxIsaRomCard1", "comboBoxIsaRomCard2", "comboBoxIsaRomCard3", "comboBoxIsaRomCard4" },
          { "pushButtonConfigureIsaRomCard1", "pushButtonConfigureIsaRomCard2", "pushButtonConfigureIsaRomCard3", "pushButtonConfigureIsaRomCard4" },
          isarom_get_device, ONBOARD_NONE, {} },
        { QT_TRANSLATE_NOOP("SettingsCards", "Real-time clock"), { "comboBoxRTC" }, { "pushButtonConfigureRTC" }, isartc_get_device, ONBOARD_NONE, {} },
        { QT_TRANSLATE_NOOP("SettingsCards", "Game port"), { "comboBoxGameportCard" }, { "pushButtonConfigureGameportCard" },
          gameport_card_get_device, ONBOARD_NONE, {}, true },
        { QT_TRANSLATE_NOOP("SettingsCards", "Port"), {}, {}, nullptr, ONBOARD_NONE,
          { { "checkBoxSerial1", nullptr, serialCard<1> }, { "checkBoxSerial2", nullptr, serialCard<2> },
            { "checkBoxSerial3", nullptr, serialCard<3> }, { "checkBoxSerial4", nullptr, serialCard<4> },
            { "checkBoxParallel1", nullptr, parallelCard<1> }, { "checkBoxParallel2", nullptr, parallelCard<2> },
            { "checkBoxParallel3", nullptr, parallelCard<3> }, { "checkBoxParallel4", nullptr, parallelCard<4> } } },
        { QT_TRANSLATE_NOOP("SettingsCards", "Expansion unit"), { "comboBoxIsaExpansion" }, { "pushButtonConfigureIsaExpansion" },
          isa_expansion_get_device, ONBOARD_NONE, {} },
        { QT_TRANSLATE_NOOP("SettingsCards", "Other"), {}, {}, nullptr, ONBOARD_NONE,
          { { "checkBoxISABugger", nullptr, [](int) -> const device_t * { return &bugger_device; } },
            { "checkBoxPOSTCard", nullptr, [](int) -> const device_t * { return &postcard_device; } },
            { "checkBoxUnitTester", "pushButtonConfigureUT", [](int) -> const device_t * { return &unittester_device; } },
            { "checkBoxKeyCard", "pushButtonConfigureKeyCard", [](int) -> const device_t * { return &novell_keycard_device; } },
            { "checkBoxSoftPower", "pushButtonConfigureSoftPower", [](int) -> const device_t * { return &softpower_device; } } } },
    };

    return list;
}

QString
categoryTitle(int category)
{
    return QCoreApplication::translate("SettingsCards", categories()[category].title);
}

QWidget *
control(const QString &name)
{
    return (Settings::settings != nullptr) ? Settings::settings->findChild<QWidget *>(name) : nullptr;
}

/* The slot a card goes in, by its bus. */
QString
busOf(const device_t *dev)
{
    if (dev == nullptr)
        return "Other";
    else if (dev->flags & DEVICE_AGP)
        return "AGP";
    else if (dev->flags & DEVICE_PCI)
        return "PCI";
    else if (dev->flags & DEVICE_VLB)
        return "VLB";
    else if (dev->flags & DEVICE_EISA)
        return "EISA";
    else if (dev->flags & (DEVICE_MCA | DEVICE_MCA32))
        return "MCA";
    else if (dev->flags & (DEVICE_ISA | DEVICE_ISA16 | DEVICE_SIDECAR))
        return "ISA";
    else if (dev->flags & DEVICE_CBUS)
        return "C-BUS";
    else if (dev->flags & DEVICE_PCMCIA)
        return "PCMCIA";

    return "Other";
}

/* A shared ISA/VLB connector is one physical position for either bus.
   EISA and 16-bit ISA positions also accept narrower ISA cards. */
bool
fits(const QString &slot, const QString &card)
{
    return (slot == card) || ((slot == "EISA" || slot == "ISA8") && (card == "ISA")) ||
        ((slot == "ISA/VLB") && (card == "ISA" || card == "VLB"));
}

static bool
fitsDevice(const QString &slot, const device_t *dev)
{
    return fits(slot, busOf(dev)) && (slot != "ISA8" || !(dev->flags & DEVICE_ISA16));
}

/* Whether the machine has slots of a group. */
static bool
machineHasGroup(int mach, const QString &group)
{
    if (group == "AGP")
        return machine_has_bus(mach, MACHINE_BUS_AGP) && !machine_has_flags(mach, MACHINE_AGP_INTERNAL);
    else if (group == "PCI")
        return machine_has_bus(mach, MACHINE_BUS_PCI) && !machine_has_flags(mach, MACHINE_PCI_INTERNAL);
    else if (group == "VLB")
        return machine_has_bus(mach, MACHINE_BUS_VLB);
    else if (group == "EISA")
        return machine_has_bus(mach, MACHINE_BUS_EISA);
    else if (group == "MCA")
        return machine_has_bus(mach, MACHINE_BUS_MCA);
    else if (group == "ISA")
        return machine_has_bus(mach, MACHINE_BUS_ISA);

    return false;
}

QString
chipsetName(int mach)
{
    /* One entry for each chipset, in order. */
    for (int i = 0; i < MACHINE_CHIPSET_MAX; i++) {
        if (machine_chipsets[i].id == static_cast<char>(machines[mach].chipset))
            return QString(machine_chipsets[i].name);
    }

    return QString();
}

/* The chip a machine's own device of a category is. */
static QString
onboardName(int mach, int onboard)
{
    const device_t *dev = nullptr;
    char            name[512];

    switch (onboard) {
        case ONBOARD_VIDEO:
            dev = machine_get_vid_device(mach);
            break;
        case ONBOARD_SOUND:
            dev = machine_get_snd_device(mach);
            break;
        case ONBOARD_NETWORK:
            dev = machine_get_net_device(mach);
            break;
        case ONBOARD_FDC:
            machine_get_fdc_chip_name(mach, name, sizeof(name));
            if (name[0] != '\0')
                return QString(name);
            break;
        case ONBOARD_HDC:
            if ((dev = machine_get_ide_device(mach)) != nullptr)
                break;
            if (machine_has_flags(mach, MACHINE_IDE_QUAD | MACHINE_IDE))
                return QCoreApplication::translate("SettingsCards", "%1 IDE").arg(chipsetName(mach));
            if (machine_has_flags(mach, MACHINE_XTA))
                return QCoreApplication::translate("SettingsCards", "XTA controller");
            if (machine_has_flags(mach, MACHINE_ESDI))
                return QCoreApplication::translate("SettingsCards", "ESDI controller");
            if (machine_has_flags(mach, MACHINE_MFM))
                return QCoreApplication::translate("SettingsCards", "MFM/RLL controller");
            /* A board whose own disk controller is its SCSI chip. */
            if (machine_has_flags(mach, MACHINE_SCSI) && ((dev = machine_get_scsi_device(mach)) != nullptr))
                break;
            break;
        default:
            break;
    }

    if (dev != nullptr)
        return DeviceConfig::DeviceName(dev, dev->internal_name, 0);

    /* A board that builds the device in its own init, with no device of it
       in its table entry (the PS/2's and PS/55's planar video): its kind. */
    switch (onboard) {
        case ONBOARD_VIDEO:
            return QCoreApplication::translate("SettingsCards", "Built-in video");
        case ONBOARD_SOUND:
            return QCoreApplication::translate("SettingsCards", "Built-in sound");
        case ONBOARD_NETWORK:
            return QCoreApplication::translate("SettingsCards", "Built-in network");
        case ONBOARD_FDC:
            return QCoreApplication::translate("SettingsCards", "Floppy controller");
        case ONBOARD_HDC:
            return QCoreApplication::translate("SettingsCards", "Disk controller");
        default:
            return QCoreApplication::translate("SettingsCards", "Internal device");
    }
}

static const device_t *
onboardDevice(int mach, int onboard)
{
    switch (onboard) {
        case ONBOARD_VIDEO:
            return machine_get_vid_device(mach);
        case ONBOARD_SOUND:
            return machine_get_snd_device(mach);
        case ONBOARD_NETWORK:
            return machine_get_net_device(mach);
        case ONBOARD_HDC:
            if (const device_t *ide = machine_get_ide_device(mach))
                return ide;
            return machine_has_flags(mach, MACHINE_SCSI) ? machine_get_scsi_device(mach) : nullptr;
        default:
            return nullptr;
    }
}

void
nameOnboardEntries()
{
    const int mach = Settings::settings->currentMachine();

    for (const auto &cat : categories()) {
        if (cat.onboard == ONBOARD_NONE)
            continue;
        const QString name = QCoreApplication::translate("SettingsCards", "[Onboard] %1").arg(onboardName(mach, cat.onboard));
        for (const auto &combo : cat.combos) {
            auto *cbox = qobject_cast<QComboBox *>(control(combo));
            if (cbox == nullptr)
                continue;
            const int row = cbox->findData(1);
            if ((row >= 0) && (cbox->itemText(row) != name))
                cbox->setItemText(row, name);
        }
    }
}

QList<FloppyController>
floppyControllers()
{
    QList<FloppyController> out;
    const int               mach = Settings::settings->currentMachine();
    const auto             *cbox = qobject_cast<QComboBox *>(control("comboBoxFD"));
    const int               id   = ((cbox != nullptr) && (cbox->currentIndex() >= 0)) ? cbox->currentData().toInt() : FDC_INTERNAL;

    if (id == 0)
        return out;
    if (id == FDC_INTERNAL) {
        out.append({ QCoreApplication::translate("SettingsCards", "[Onboard] %1").arg(onboardName(mach, ONBOARD_FDC)), 0,
                     machine_get_fdc_drives(mach) });
        return out;
    }

    const device_t *dev = fdc_card_getdevice(id);
    out.append({ cbox->currentText(), 0, fdc_device_drives(dev) });
    /* The Monster FDC's second controller, when it is enabled. */
    if (dev == &fdc_monster_device) {
        device_context_t context;
        device_set_context(&context, dev, 0);
        if (config_get_int(context.name, "sec_enabled", 0))
            out.append({ QCoreApplication::translate("SettingsCards", "%1 (secondary controller)").arg(cbox->currentText()), 4,
                         fdc_device_drives(dev) });
    }

    return out;
}

int
floppyControllerOf(const QList<FloppyController> &controllers, int drive, int *number)
{
    for (int c = 0; c < controllers.size(); c++) {
        const int n = drive - controllers[c].first;
        if ((n >= 0) && (n < controllers[c].drives)) {
            if (number != nullptr)
                *number = n + 1;
            return c;
        }
    }

    return -1;
}

/* Soft power is a card on a machine with ISA slots, and the board's own
   everywhere else. */
static bool
softPowerOnboard(int mach)
{
    return (machines[mach].init == machine_ibm5140_init) || !machine_has_bus(mach, MACHINE_BUS_ISA);
}

/* Every card chosen on the classic pages, with the bus it plugs into. */
QList<Card>
list(int only)
{
    QList<Card> out;
    const int   mach = Settings::settings->currentMachine();
    const auto &cats = categories();

    for (int c = 0; c < cats.size(); c++) {
        if ((only >= 0) && (c != only))
            continue;

        for (int i = 0; i < cats[c].combos.size(); i++) {
            const auto *cbox = qobject_cast<QComboBox *>(control(cats[c].combos[i]));
            if ((cbox == nullptr) || (cbox->currentIndex() < 0))
                continue;

            const int id = cbox->currentData().toInt();
            if ((id <= 0) || (cats[c].automatic && (id == 1)))
                continue;

            Card card;
            card.category = c;
            card.control  = cats[c].combos[i];
            card.check    = false;
            card.index    = i;
            card.onboard  = (cats[c].onboard != ONBOARD_NONE) && (id == 1);
            if (card.onboard) {
                card.name     = QCoreApplication::translate("SettingsCards", "[Onboard] %1").arg(onboardName(mach, cats[c].onboard));
                card.dev      = onboardDevice(mach, cats[c].onboard);
                card.instance = ((cats[c].onboard == ONBOARD_NETWORK) && machine_has_flags_64(mach, MACHINE_NIC_SEC)) ? 1 : 0;
            } else {
                card.name     = cbox->currentText();
                card.dev      = cats[c].device(id);
                card.bus      = busOf(card.dev);
                card.instance = (cats[c].combos.size() == 1) ? 0 : (i + 1);
            }
            out.append(card);
        }

        for (int i = 0; i < cats[c].checks.size(); i++) {
            const auto *cbox = qobject_cast<QCheckBox *>(control(cats[c].checks[i].control));
            if ((cbox == nullptr) || !cbox->isEnabled() || !cbox->isChecked() || (cats[c].checks[i].device(mach) == nullptr))
                continue;

            Card card;
            card.category = c;
            card.control  = cats[c].checks[i].control;
            card.check    = true;
            card.index    = i;
            card.dev      = cats[c].checks[i].device(mach);
            card.instance = 0;
            card.onboard  = (card.dev == &softpower_device) && softPowerOnboard(mach);
            if (card.onboard)
                card.name = QCoreApplication::translate("SettingsCards", "[Onboard] %1").arg(DeviceConfig::DeviceName(card.dev, card.dev->internal_name, 0));
            else {
                card.name = DeviceConfig::DeviceName(card.dev, card.dev->internal_name, 1);
                card.bus  = busOf(card.dev);
            }
            if (isPortCard(card.dev))
                card.name += QString(" (%1%2)").arg(isSerialCard(card.dev) ? "COM" : "LPT", card.control.right(1));
            out.append(card);
        }
    }

    return out;
}

bool
onboardChoice(int category, Choice *choice, Card *displaced)
{
    const auto &cat  = categories()[category];
    const int   mach = Settings::settings->currentMachine();

    if (cat.onboard == ONBOARD_NONE)
        return false;

    /* The classic page offers the board's own device in the controls that
       can hold it: in use when one holds it, else the first free one, else
       the first holding a card. */
    QComboBox *free = nullptr;
    QComboBox *held = nullptr;
    for (const auto &name : cat.combos) {
        auto *cbox = qobject_cast<QComboBox *>(control(name));
        if ((cbox == nullptr) || (cbox->currentIndex() < 0))
            continue;
        if (cbox->currentData().toInt() == 1)
            return false;
        if (!cbox->isEnabled() || (cbox->findData(1) < 0))
            continue;
        if ((free == nullptr) && (cbox->currentData().toInt() == 0))
            free = cbox;
        if ((held == nullptr) && (cbox->currentData().toInt() > 1))
            held = cbox;
    }
    if (free == nullptr) {
        if ((held == nullptr) || (displaced == nullptr))
            return false;
        bool found = false;
        for (const auto &card : list(category)) {
            if (!card.check && (card.control == held->objectName())) {
                *displaced = card;
                found      = true;
            }
        }
        if (!found)
            return false;
        free = held;
    } else if (displaced != nullptr)
        displaced->control.clear();

    *choice         = { category, free->objectName(), false, 1,
                        QCoreApplication::translate("SettingsCards", "[Onboard] %1").arg(onboardName(mach, cat.onboard)),
                        onboardDevice(mach, cat.onboard) };
    choice->onboard = true;
    return true;
}

QList<Choice>
choices(int only, const QString &bus, const Card *replacing)
{
    QList<Choice> out;
    const int     mach = Settings::settings->currentMachine();
    const auto   &cats = categories();

    for (int c = 0; c < cats.size(); c++) {
        if ((only >= 0) && (c != only))
            continue;

        Choice own;
        if (bus.isEmpty() && onboardChoice(c, &own))
            out.append(own);

        /* The first control of the kind with nothing in it. */
        QComboBox *free = nullptr;
        for (const auto &name : cats[c].combos) {
            auto *cbox = qobject_cast<QComboBox *>(control(name));
            if ((cbox != nullptr) && cbox->isEnabled() && (cbox->currentIndex() >= 0) && (cbox->currentData().toInt() == 0)) {
                free = cbox;
                break;
            }
        }
        if (!free && replacing && replacing->category == c && !replacing->check)
            free = qobject_cast<QComboBox *>(control(replacing->control));
        if (free != nullptr) {
            for (int row = 0; row < free->count(); row++) {
                const int id = free->itemData(row).toInt();
                if ((id <= 0) || (((cats[c].onboard != ONBOARD_NONE) || cats[c].automatic) && (id == 1)))
                    continue;

                const device_t *dev     = cats[c].device(id);
                const QString   cardBus = busOf(dev);
                if (bus.isEmpty() ? (!machineHasGroup(mach, cardBus) && (cardBus != "Other")) : !fitsDevice(bus, dev))
                    continue;

                out.append({ c, free->objectName(), false, id, free->itemText(row), dev });
            }
        }

        QSet<const device_t *> offered;
        for (const auto &check : cats[c].checks) {
            auto *cbox = qobject_cast<QCheckBox *>(control(check.control));
            if ((cbox == nullptr) || !cbox->isEnabled() || cbox->isChecked())
                continue;

            const device_t *dev = check.device(mach);
            /* One port card of each kind: the lowest free port. */
            if ((dev == nullptr) || offered.contains(dev))
                continue;
            if (isPortCard(dev)) {
                offered.insert(dev);
                const QString port = QString("%1%2").arg(isSerialCard(dev) ? "COM" : "LPT", QString(check.control).right(1));
                out.append({ c, QString(check.control), true, 1,
                             QString("%1 (%2)").arg(DeviceConfig::DeviceName(dev, dev->internal_name, 1), port), dev });
                continue;
            }
            if ((dev == &softpower_device) && softPowerOnboard(mach))
                continue;
            if (!bus.isEmpty() && !fitsDevice(bus, dev))
                continue;

            out.append({ c, QString(check.control), true, 1, DeviceConfig::DeviceName(dev, dev->internal_name, 1), dev });
        }
    }

    return out;
}

/* A board layout entry with a PCI bus number of its own behind it: an AGP
   or PCI-to-PCI bridge, or a second host bridge. */
static bool
boardBusBridge(const machine_slot_t *s)
{
    return (s->kind == SLOT_PCI) &&
           ((s->pci_type == PCI_CARD_AGPBRIDGE) || (s->pci_type == PCI_CARD_BRIDGE) || (s->pci_type == PCI_CARD_NORTHBRIDGE_AUX));
}

/* How many slots of each bus the board's layout has, bridges included;
   -1 where a count is not recorded. */
static void
countSlots(const machine_slot_t *bus, QMap<QString, int> &slotsOf)
{
    for (const machine_slot_t *s = bus; s->kind != SLOT_END; s++) {
        QString name;
        switch (s->kind) {
            case SLOT_PCI:
                if (s->pci_type == PCI_CARD_NORMAL)
                    slotsOf["PCI"] = qMax(slotsOf.value("PCI", 0), 0) + 1;
                if (s->bus != nullptr)
                    countSlots(s->bus, slotsOf);
                continue;
            case SLOT_AGP:
                name = "AGP";
                break;
            case SLOT_ISA8:
                name = "ISA8";
                break;
            case SLOT_ISA16:
                name = "ISA16";
                break;
            case SLOT_ISA_VLB:
                name = "ISA/VLB";
                break;
            case SLOT_VLB:
                name = "VLB";
                break;
            case SLOT_EISA:
                name = "EISA";
                break;
            case SLOT_MCA:
            case SLOT_MCA_INTERNAL: /* a slot all the same, for what fits */
                name = "MCA";
                break;
            default:
                continue;
        }
        auto addCount = [&](const QString &key) {
            if (s->count == 0)
                slotsOf[key] = -1;
            else if (slotsOf.value(key, 0) >= 0)
                slotsOf[key] = slotsOf.value(key, 0) + s->count;
        };
        addCount(name);
        if (name == "ISA8" || name == "ISA16" || name == "ISA/VLB")
            addCount("ISA");
        if (name == "ISA/VLB")
            addCount("VLB");
    }
}

QString
fitError(const device_t *adding, const Card *without)
{
    const int        mach = Settings::settings->currentMachine();
    const machine_t *m    = &machines[mach];
    const QString    bus  = busOf(adding);

    /* PCI cards past the slots go behind a bridge 86Box adds, on a board
       that takes one; the rest fit where the count of their slots is not
       known. */
    if ((bus == "PCI") && (m->slot_layout != nullptr) && (m->pci_flags & FLAG_NO_BRIDGES)) {
        QMap<QString, int> counted;
        countSlots(m->slot_layout, counted);
        int pci = 0;
        for (const auto &card : list()) {
            if (!card.onboard && (card.bus == "PCI") &&
                ((without == nullptr) || (card.control != without->control) || (card.check != without->check)))
                pci++;
        }
        const QString name = DeviceConfig::DeviceName(adding, adding->internal_name, 0);
        if (counted.value("PCI", 0) == 0)
            return QCoreApplication::translate("SettingsCards", "%1 needs a PCI slot, and this machine has none.").arg(name);
        if ((pci + 1) > counted.value("PCI", 0))
            return QCoreApplication::translate("SettingsCards", "%1 needs a PCI slot, and all %2 of this machine's PCI slots hold cards. This board takes no PCI-PCI bridge.")
                .arg(name)
                .arg(counted.value("PCI", 0));
        return QString();
    }
    if ((m->slot_layout == nullptr) || (bus == "PCI") || (bus == "Other"))
        return QString();

    QMap<QString, int> slotsOf;
    countSlots(m->slot_layout, slotsOf);
    /* An expansion unit's slots, the one in or the one going in. */
    const device_t *unit = (isa_expansion_slots(adding) > 0) ? adding : nullptr;
    for (const auto &card : list(CATEGORY_EXPANSION)) {
        if ((unit == nullptr) && ((without == nullptr) || (card.control != without->control)))
            unit = card.dev;
    }
    if (unit == nullptr)
        unit = machineExpansionUnit(mach);
    if (unit != nullptr) {
        const QString kind = (isa_expansion_slot_kind(unit) == SLOT_ISA16) ? "ISA16" : "ISA8";
        for (const QString &key : { kind, QString("ISA") }) {
            if (slotsOf.value(key, 0) >= 0)
                slotsOf[key] = slotsOf.value(key, 0) + isa_expansion_slots(unit);
        }
    }

    auto capacity = [&](const QString &b) {
        if (slotsOf.contains(b))
            return slotsOf[b];
        if ((b == "ISA") && slotsOf.contains("EISA"))
            return 0;
        return -1;
    };

    QMap<QString, int> need;
    int needIsa16 = 0;
    for (const auto &card : list()) {
        if (card.onboard)
            continue;
        if ((without != nullptr) && (card.control == without->control) && (card.check == without->check))
            continue;
        need[card.bus]++;
        if (card.bus == "ISA" && (card.dev->flags & DEVICE_ISA16))
            needIsa16++;
    }
    need[bus]++;
    if (bus == "ISA" && (adding->flags & DEVICE_ISA16))
        needIsa16++;

    const QString name = DeviceConfig::DeviceName(adding, adding->internal_name, 0);

    if ((bus == "ISA" || bus == "VLB" || bus == "EISA") &&
        (slotsOf.contains("ISA8") || slotsOf.contains("ISA/VLB"))) {
        const int isa = capacity("ISA");
        const int isa16 = slotsOf.value("ISA16", 0);
        const int shared = slotsOf.value("ISA/VLB", 0);
        const int vlb = slotsOf.value("VLB", 0);
        const int eisa = slotsOf.value("EISA", 0);
        if (isa < 0 || isa16 < 0 || shared < 0 || vlb < 0 || eisa < 0)
            return QString();
        const int vlbOnly = vlb - shared;
        const int eisaFree = qMax(eisa - need["EISA"], 0);
        if (need["EISA"] <= eisa && need["VLB"] <= vlb &&
            needIsa16 + need["VLB"] <= isa16 + shared + vlbOnly + eisaFree &&
            need["ISA"] + need["VLB"] <= isa + vlbOnly + eisaFree)
            return QString();
        return QCoreApplication::translate("SettingsCards", "%1 needs a free compatible physical slot. ISA/VLB connectors can hold only one card each.").arg(name);
    }

    if ((bus == "ISA") || (bus == "EISA")) {
        const int isa  = capacity("ISA");
        const int eisa = capacity("EISA");
        if ((isa < 0) || (eisa < 0))
            return QString();

        /* EISA cards in EISA slots; ISA cards in ISA slots, then in the
           EISA slots left. */
        const int overflow = qMax(need["ISA"] - isa, 0);
        if ((need["EISA"] + overflow) <= eisa)
            return QString();

        if (bus == "EISA")
            return QCoreApplication::translate("SettingsCards", "%1 needs an EISA slot, and all %2 of this machine's EISA slots hold cards.")
                .arg(name).arg(eisa);
        if (isa == 0)
            return QCoreApplication::translate("SettingsCards", "%1 needs an ISA slot. This machine has no ISA slots, and all %2 of its EISA slots, which take ISA cards, hold cards.")
                .arg(name).arg(eisa);
        return QCoreApplication::translate("SettingsCards", "%1 needs an ISA slot, and all of this machine's slots that take one (%2 ISA, %3 EISA) hold cards.")
            .arg(name).arg(isa).arg(eisa);
    }

    const int cap = capacity(bus);
    if ((cap < 0) || (need[bus] <= cap))
        return QString();

    if (cap == 0)
        return QCoreApplication::translate("SettingsCards", "%1 needs a %2 slot, and this machine has none.").arg(name, bus);
    return QCoreApplication::translate("SettingsCards", "%1 needs a %2 slot, and all %3 of this machine's %2 slots hold cards.")
        .arg(name, bus).arg(cap);
}

/* The first video slot is never left empty with a card in the second:
   video_reset() adds no card past an empty first slot, as upstream's
   does. The second card moves up, and its placement with it. */
static void
promoteSecondVideo()
{
    auto *first  = qobject_cast<QComboBox *>(control("comboBoxVideo"));
    auto *second = qobject_cast<QComboBox *>(control("comboBoxVideoSecondary"));

    if ((first == nullptr) || (second == nullptr) || (first->currentData().toInt() != VID_NONE))
        return;

    const int id  = second->currentData().toInt();
    const int row = first->findData(id);
    if ((id <= VID_EXTERNAL) || (row < 0))
        return;

    const device_t *dev   = video_card_getdevice(id);
    const int       slot  = assignments.value(deviceKey(dev, 2), -1);
    const int       empty = second->findData(VID_NONE);

    first->setCurrentIndex(row);
    if (empty >= 0)
        second->setCurrentIndex(empty);
    assignments.remove(deviceKey(dev, 2));
    if (slot >= 0)
        assignments.insert(deviceKey(dev, 1), slot);
    assignmentsEdited = true;
}

void
remove(const Card &card)
{
    const auto before = list();
    for (const auto &node : layout(before))
        if (node.card >= 0 && node.slot >= 0)
            assign(before[node.card], node.slot);
    if (card.dev) {
        assignments.remove(deviceKey(card.dev, card.instance));
        assignmentsEdited = true;
    }
    if (card.check) {
        if (auto *cbox = qobject_cast<QCheckBox *>(control(card.control)))
            cbox->setChecked(false);
    } else if (auto *cbox = qobject_cast<QComboBox *>(control(card.control))) {
        const int none = cbox->findData(0);
        if (none >= 0)
            cbox->setCurrentIndex(none);
        if ((card.category == CATEGORY_VIDEO) && (card.control == "comboBoxVideo"))
            promoteSecondVideo();
    }
}

/* Add-in cards can be removed even when the resulting machine will not
   boot; the board's own device where it can be turned off, as the classic
   page lets it be set to none. */
bool
removable(const Card &card)
{
    if (!card.onboard)
        return true;
    if (card.check)
        return false;

    const auto *cbox = qobject_cast<QComboBox *>(control(card.control));
    return (cbox != nullptr) && cbox->isEnabled() && (cbox->findData(0) >= 0);
}

bool
put(const Choice &choice, const Card *replacing, QWidget *parent, int slot)
{
    const Card   *out   = ((replacing != nullptr) && !replacing->onboard) ? replacing : nullptr;
    const QString error = choice.onboard ? QString() : fitError(choice.dev, out);

    if (!error.isEmpty()) {
        QMessageBox::warning(parent, QCoreApplication::translate("SettingsCards", "It Doesn't Fit!"), error);
        return false;
    }

    /* A card replacing one of its own kind takes its control; any other
       replaced card comes out. */
    QString where = choice.control;
    if ((out != nullptr) && !out->check && !choice.check && (out->category == choice.category))
        where = out->control;

    const auto before = list();
    for (const auto &node : layout(before))
        if (node.card >= 0 && node.slot >= 0)
            assign(before[node.card], node.slot);
    if (slot < 0 && out && out->dev)
        slot = placement(out->dev, out->instance);
    if (out && (out->check || choice.check || out->category != choice.category))
        remove(*out);
    if (out && out->dev)
        assignments.remove(deviceKey(out->dev, out->instance));
    assignmentsEdited = true;
    if (choice.check) {
        if (auto *cbox = qobject_cast<QCheckBox *>(control(where)))
            cbox->setChecked(true);
    } else if (auto *cbox = qobject_cast<QComboBox *>(control(where))) {
        const int row = cbox->findData(choice.id);
        if (row >= 0)
            cbox->setCurrentIndex(row);
    }

    // Pin every existing card before allocating the new one.
    const auto current = list();
    if (slot >= 0) {
        for (const auto &card : current)
            if (card.control == where && card.check == choice.check)
                assign(card, slot);
    }
    const auto nodes = layout(current);
    for (const auto &node : nodes)
        if (node.card >= 0 && node.slot >= 0)
            assign(current[node.card], node.slot);
    return true;
}

bool
fillAddMenu(QMenu *menu, int category, const QString &bus, QWidget *parent)
{
    const auto list = choices(category, bus);

    if (list.isEmpty()) {
        menu->addAction(QCoreApplication::translate("SettingsCards", "(none left to add)"))->setEnabled(false);
        return false;
    }

    for (const auto &choice : list)
        menu->addAction(QString(choice.name).replace("&", "&&"), parent, [choice, parent]() { put(choice, nullptr, parent); });

    return true;
}

/* Typing while the card list has the focus goes to its filter. */
namespace {
class TypeToFilter : public QObject {
public:
    TypeToFilter(QLineEdit *filter, QObject *parent)
        : QObject(parent)
        , filter(filter)
    {
    }

protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (event->type() == QEvent::KeyPress) {
            const auto *key = static_cast<QKeyEvent *>(event);
            if (!key->text().isEmpty() && key->text().at(0).isPrint()) {
                filter->setFocus();
                filter->insert(key->text());
                return true;
            }
        }
        return QObject::eventFilter(watched, event);
    }

private:
    QLineEdit *filter;
};
}

/* Down in the filter box goes on into the list. */
class DownToList : public QObject {
public:
    DownToList(QTreeWidget *tree, QObject *parent)
        : QObject(parent)
        , tree(tree)
    {
    }

protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if ((event->type() == QEvent::KeyPress) && (static_cast<QKeyEvent *>(event)->key() == Qt::Key_Down)) {
            tree->setFocus();
            if (tree->currentItem() == nullptr) {
                QTreeWidgetItemIterator it(tree, QTreeWidgetItemIterator::NotHidden);
                if (*it != nullptr)
                    tree->setCurrentItem(*it);
            }
            return true;
        }
        return QObject::eventFilter(watched, event);
    }

private:
    QTreeWidget *tree;
};

/* An item of the list shown if its name, or a group it is under, holds
   the text; a group shown if anything under it is, opened when narrowed. */
static bool
narrowTo(QTreeWidgetItem *item, const QString &text, bool under = false)
{
    const bool matches = under || text.isEmpty() || item->text(0).contains(text, Qt::CaseInsensitive);

    if (item->childCount() == 0) {
        item->setHidden(!matches);
        return matches;
    }

    bool any = false;
    for (int c = 0; c < item->childCount(); c++)
        any |= narrowTo(item->child(c), text, matches && !text.isEmpty());
    item->setHidden(!any);
    item->setExpanded(!text.isEmpty() && any);
    return any;
}

void
pickFrom(const QList<PickItem> &items, QWidget *anchor, bool dropdown)
{
    auto *popup  = new QDialog(anchor->window(), dropdown ? Qt::Popup : Qt::Dialog);
    popup->setWindowTitle(QCoreApplication::translate("SettingsCards", "Add hardware"));
    if (!dropdown)
        popup->setWindowModality(Qt::WindowModal);
    /* A drop-down has no window frame: its own edge, so it does not run
       into the page under it. */
    QWidget *body = popup;
    if (dropdown) {
        auto *outer = new QVBoxLayout(popup);
        auto *frame = new QFrame(popup);
        outer->setContentsMargins(0, 0, 0, 0);
        frame->setFrameShape(QFrame::StyledPanel);
        frame->setFrameShadow(QFrame::Raised);
        outer->addWidget(frame);
        body = frame;
    }
    auto *layout = new QVBoxLayout(body);
    auto *filter = new QLineEdit(popup);
    auto *tree   = new QTreeWidget(popup);

    popup->setAttribute(Qt::WA_DeleteOnClose);
    layout->setContentsMargins(12, 12, 12, 12);
    auto *prompt = new QLabel(QCoreApplication::translate("SettingsCards", "Find hardware by name, category, or bus."), popup);
    layout->addWidget(prompt);
    filter->setPlaceholderText(QCoreApplication::translate("SettingsCards", "Search hardware..."));
    filter->setClearButtonEnabled(true);
    tree->setHeaderHidden(true);
    tree->setRootIsDecorated(true);
    layout->addWidget(filter);
    tree->setAlternatingRowColors(true);
    layout->addWidget(tree);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, popup);
    auto *add = buttons->addButton(QCoreApplication::translate("SettingsCards", "Add"), QDialogButtonBox::AcceptRole);
    add->setEnabled(false);
    add->setDefault(false);
    layout->addWidget(buttons);
    QObject::connect(buttons, &QDialogButtonBox::rejected, popup, &QDialog::reject);
    QObject::connect(tree, &QTreeWidget::currentItemChanged, popup, [add](QTreeWidgetItem *item) {
        add->setEnabled(item && item->childCount() == 0 && item->data(0, Qt::UserRole).isValid());
    });
    tree->installEventFilter(new TypeToFilter(filter, tree));

    /* The groups made once each, in the order they first come. */
    QHash<QString, QTreeWidgetItem *> groups;
    for (int i = 0; i < items.size(); i++) {
        QTreeWidgetItem *parent = nullptr;
        QString          path;
        for (const auto &name : items[i].groups) {
            path += name + QChar(0);
            auto *group = groups.value(path);
            if (group == nullptr) {
                group = (parent != nullptr) ? new QTreeWidgetItem(parent, { name }) : new QTreeWidgetItem(tree, { name });
                group->setFlags(Qt::ItemIsEnabled);
                groups.insert(path, group);
            }
            parent = group;
        }
        auto *item = (parent != nullptr) ? new QTreeWidgetItem(parent, { items[i].name }) : new QTreeWidgetItem(tree, { items[i].name });
        item->setData(0, Qt::UserRole, i);
    }
    if (items.isEmpty()) {
        auto *none = new QTreeWidgetItem(tree, { QCoreApplication::translate("SettingsCards", "(none left to add)") });
        none->setFlags(Qt::NoItemFlags);
    }
    /* A group alone at its level opens by itself: one click less where
       there is nothing to choose between (a single bus). */
    for (QTreeWidgetItem *only = (tree->topLevelItemCount() == 1) ? tree->topLevelItem(0) : nullptr; (only != nullptr) && (only->childCount() > 0);
         only = (only->childCount() == 1) ? only->child(0) : nullptr)
        only->setExpanded(true);

    QObject::connect(filter, &QLineEdit::textChanged, tree, [tree](const QString &text) {
        for (int g = 0; g < tree->topLevelItemCount(); g++)
            narrowTo(tree->topLevelItem(g), text);
    });
    /* Down, or Enter, from the filter goes into the list. */
    QObject::connect(filter, &QLineEdit::returnPressed, tree, [tree]() {
        tree->setFocus();
        for (QTreeWidgetItemIterator it(tree, QTreeWidgetItemIterator::NotHidden); *it != nullptr; ++it) {
            if ((*it)->childCount() == 0) {
                tree->setCurrentItem(*it);
                break;
            }
        }
    });
    filter->installEventFilter(new DownToList(tree, filter));
    QObject::connect(tree, &QTreeWidget::itemClicked, tree, [](QTreeWidgetItem *item, int) {
        if (item->childCount() > 0)
            item->setExpanded(!item->isExpanded());
    });

    auto choose = [popup, items, anchor](QTreeWidgetItem *item) {
        if ((item == nullptr) || (item->childCount() > 0) || !item->data(0, Qt::UserRole).isValid())
            return;
        const auto action = items[item->data(0, Qt::UserRole).toInt()].choose;
        popup->close();
        QTimer::singleShot(0, anchor->window(), [action]() { action(); });
    };
    QObject::connect(tree, &QTreeWidget::itemActivated, popup, [choose](QTreeWidgetItem *item, int) { choose(item); });
    QObject::connect(add, &QPushButton::clicked, popup, [choose, tree]() { choose(tree->currentItem()); });

    popup->resize(560, 480);
    popup->setMinimumSize(400, 280);
    if (dropdown) {
        const QRect screen = anchor->screen()->availableGeometry();
        QPoint pos = anchor->mapToGlobal(QPoint(0, anchor->height()));
        const int below = screen.bottom() - pos.y() - 8;
        if (below < 280)
            pos.setY(qMax(screen.top(), anchor->mapToGlobal(QPoint()).y() - 480));
        else
            popup->resize(560, qMin(480, below));
        pos.setX(qBound(screen.left(), pos.x(), qMax(screen.left(), screen.right() - popup->width())));
        popup->move(pos);
    }
    popup->show();
    filter->setFocus();
}

/* Kind, then bus, then the cards: Video > ISA > the ISA video cards. A
   page of one kind starts at the bus. */
void
pickCard(int category, const QString &bus, QWidget *anchor)
{
    static const QStringList busOrder = { "ISA", "EISA", "VLB", "MCA", "PCI", "AGP", "C-BUS", "PCMCIA", "Other" };
    const auto               list     = choices(category, bus);

    QList<int> order;
    for (int i = 0; i < list.size(); i++)
        order.append(i);
    std::stable_sort(order.begin(), order.end(), [&list](int a, int b) {
        if (list[a].category != list[b].category)
            return list[a].category < list[b].category;
        if (list[a].onboard != list[b].onboard)
            return list[a].onboard;
        return busOrder.indexOf(busOf(list[a].dev)) < busOrder.indexOf(busOf(list[b].dev));
    });

    QList<PickItem> items;
    for (const int i : order) {
        QStringList groups;
        if (category < 0)
            groups.append(categoryTitle(list[i].category));
        groups.append(list[i].onboard ? QCoreApplication::translate("SettingsCards", "Onboard") : busOf(list[i].dev));
        const Choice choice = list[i];
        QWidget     *parent = anchor->window();
        items.append({ groups, list[i].name, [choice, parent]() { put(choice, nullptr, parent); } });
    }
    if ((category < 0) && bus.isEmpty() && (bridgeSlot() >= 0))
        items.append({ { QCoreApplication::translate("SettingsCards", "Bridges") },
                       QCoreApplication::translate("SettingsCards", "PCI-to-PCI bridge (nine more PCI slots behind it)"),
                       []() {
                           const int slot = bridgeSlot();
                           if (slot >= 0)
                               addBridge(slot);
                       } });

    pickFrom(items, anchor, true);
}

static QPushButton *
buttonFor(const Card &card)
{
    const auto &cat = categories()[card.category];
    const char *name = card.check ? cat.checks[card.index].button : nullptr;

    if (!card.check)
        return qobject_cast<QPushButton *>(control(cat.buttons.value(card.index)));

    return (name != nullptr) ? qobject_cast<QPushButton *>(control(name)) : nullptr;
}

QStringList
portUsers(int com)
{
    QStringList users;

    for (const auto &[name, get] : { std::pair<const char *, const device_t *(*)(int)> { "comboBoxMouse", mouse_get_device },
                                     std::pair<const char *, const device_t *(*)(int)> { "comboBoxTablet", tablet_get_device } }) {
        const auto *cbox = qobject_cast<QComboBox *>(control(name));
        if ((cbox == nullptr) || (cbox->currentIndex() < 0))
            continue;
        const device_t *dev = get(cbox->currentData().toInt());
        if ((dev == nullptr) || (dev->config == nullptr))
            continue;
        for (const auto *c = dev->config; c->type != CONFIG_END; c++) {
            if (strcmp(c->name, "port"))
                continue;
            device_context_t context;
            device_set_context(&context, dev, 0);
            if (config_get_int(context.name, "port", c->default_int) == (com - 1))
                users.append(DeviceConfig::DeviceName(dev, dev->internal_name, 0));
        }
    }

    return users;
}

/* A port card's options: what is attached to its port, as the Ports
   page has it, and the input devices that use the port. */
static void
configurePort(const Card &card)
{
    const bool    serial = isSerialCard(card.dev);
    const QString n      = card.control.right(1);
    auto         *source = qobject_cast<QComboBox *>(control(QString(serial ? "comboBoxCom%1" : "comboBoxLpt%1").arg(n)));
    auto         *button = qobject_cast<QPushButton *>(control(QString(serial ? "pushButtonConfigureCom%1" : "pushButtonConfigureLpt%1").arg(n)));
    if (source == nullptr)
        return;

    QDialog dialog(Settings::settings);
    dialog.setWindowTitle(card.name);
    auto *layout = new QVBoxLayout(&dialog);
    auto *row    = new QHBoxLayout();
    auto *cbox   = new QComboBox(&dialog);
    cbox->setModel(source->model());
    cbox->setCurrentIndex(source->currentIndex());
    QObject::connect(cbox, QOverload<int>::of(&QComboBox::activated), source, [source](int index) { source->setCurrentIndex(index); });
    row->addWidget(new QLabel(QCoreApplication::translate("SettingsCards", "Attached:"), &dialog));
    row->addWidget(cbox, 1);
    if (button != nullptr) {
        auto *conf = new QPushButton(QCoreApplication::translate("SettingsCards", "Configure"), &dialog);
        conf->setEnabled(button->isEnabled());
        QObject::connect(source, QOverload<int>::of(&QComboBox::currentIndexChanged), conf, [conf, button]() { conf->setEnabled(button->isEnabled()); });
        QObject::connect(conf, &QPushButton::clicked, button, [button]() { button->click(); });
        row->addWidget(conf);
    }
    layout->addLayout(row);
    if (serial) {
        const auto users = portUsers(n.toInt());
        if (!users.isEmpty())
            layout->addWidget(new QLabel(QCoreApplication::translate("SettingsCards", "Also on this port, chosen in its own options: %1").arg(users.join(", ")), &dialog));
    }
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    dialog.exec();
}

bool
configurable(const Card &card)
{
    if (isPortCard(card.dev))
        return true;

    const auto *button = buttonFor(card);

    return (button != nullptr) && button->isEnabled();
}

void
configure(const Card &card)
{
    if (isPortCard(card.dev)) {
        configurePort(card);
        return;
    }
    if (auto *button = buttonFor(card); (button != nullptr) && button->isEnabled())
        button->click();
}

static QWidget                                *panelHolder = nullptr;
static QHash<QString, QPointer<DeviceConfigPanel>> panelPool;

static QString
panelKey(const Card &card)
{
    return QString("%1/%2").arg(reinterpret_cast<quintptr>(card.dev)).arg(card.instance);
}

void
setPanelHolder(QWidget *holder)
{
    panelHolder = holder;
    panelPool.clear();
}

DeviceConfigPanel *
panel(const Card &card)
{
    if ((card.dev == nullptr) || (card.dev->config == nullptr))
        return nullptr;

    const QString key = panelKey(card);
    if (auto existing = panelPool.value(key); !existing.isNull())
        return existing;

    auto *made = new DeviceConfigPanel(card.dev, card.instance, qobject_cast<Settings *>(Settings::settings), panelHolder);
    made->hide();
    panelPool.insert(key, made);

    return made;
}

void
releasePanel(DeviceConfigPanel *panel)
{
    if ((panel == nullptr) || (panelHolder == nullptr))
        return;

    panel->hide();
    panel->setParent(panelHolder);
}

/* The panels of cards no longer there go. */
void
prunePanels()
{
    QSet<QString> keep;
    for (const auto &card : list())
        keep.insert(panelKey(card));

    for (auto it = panelPool.begin(); it != panelPool.end();) {
        if (it.value().isNull())
            it = panelPool.erase(it);
        else if (!keep.contains(it.key())) {
            it.value()->deleteLater();
            it = panelPool.erase(it);
        } else
            ++it;
    }
}

QList<Node>
layout(const QList<Card> &cards)
{
    followMachine();
    const int        mach = Settings::settings->currentMachine();
    const machine_t *m    = &machines[mach];
    QList<Node>      nodes;
    QVector<bool>    placed(cards.size(), false); /* QList has no (size, value) constructor in Qt 5 */

    auto add = [&nodes](Node::Kind kind, int parent, const QString &label, const QString &bus = QString(), int pciType = -1) {
        nodes.append({ kind, parent, label, bus, pciType, -1 });
        return static_cast<int>(nodes.size() - 1);
    };
    auto take = [&](const QString &bus) {
        for (int i = 0; i < cards.size(); i++) {
            if (!placed[i] && !cards[i].onboard && fitsDevice(bus, cards[i].dev) && placement(cards[i].dev, cards[i].instance) < 0) {
                placed[i] = true;
                return i;
            }
        }
        return -1;
    };
    QHash<QString, int> slotNumbers;
    auto slotsOf = [&](int parent, const QString &bus, int count) {
        if (count > 0) {
            static const QMap<QString, int> kinds = { { "ISA", SLOT_ISA16 }, { "ISA8", SLOT_ISA8 }, { "ISA/VLB", SLOT_ISA_VLB },
                                                       { "EISA", SLOT_EISA }, { "MCA", SLOT_MCA }, { "VLB", SLOT_VLB }, { "AGP", SLOT_AGP } };
            for (int i = 1; i <= count; i++) {
                const int number = ++slotNumbers[(bus == "ISA8" || bus == "ISA/VLB") ? "ISA" : bus];
                QString label = QCoreApplication::translate("SettingsCards", "%1 Slot %2").arg(bus == "ISA8" ? "ISA" : bus).arg(number);
                if (bus == "ISA8")
                    label += QCoreApplication::translate("SettingsCards", " (8-bit)");
                auto &node = nodes[add(Node::SLOT, parent, label, bus)];
                node.slot = (kinds.value(bus) << 16) | number;
            }
            return;
        }
        for (int c; (c = take(bus)) >= 0;)
            nodes[add(Node::SLOT, parent, QCoreApplication::translate("SettingsCards", "%1 — automatic position").arg(bus), bus)].card = c;
        add(Node::SLOT, parent, QCoreApplication::translate("SettingsCards", "%1 slot (count unknown)").arg(bus), bus);
    };

    /* PCI slots are numbered across the whole board, as boards label them;
       a bus behind a bridge is the next bus number, in the layout's order,
       as the emulation registers them. */
    int pci     = 0;
    int nextBus = 1;
    std::function<void(int, const machine_slot_t *, int)> walk = [&](int parent, const machine_slot_t *bus, int busIndex) {
        for (const machine_slot_t *s = bus; s->kind != SLOT_END; s++) {
            if (s->if_bios && m->device) {
                device_context_t context;
                device_set_context(&context, m->device, 0);
                const char *bios = config_get_string(context.name, "bios", const_cast<char *>(""));
                const bool negate = s->if_bios[0] == '!';
                if ((strcmp(bios, s->if_bios + negate) == 0) == negate)
                    continue;
            }
            const QString dev = QString("%1h").arg(s->pci_dev, 2, 16, QChar('0')).toUpper();

            switch (s->kind) {
                case SLOT_PCI:
                    switch (s->pci_type) {
                        case PCI_CARD_NORMAL:
                            nodes[add(Node::SLOT, parent, QCoreApplication::translate("SettingsCards", "PCI Slot %1").arg(++pci), "PCI")].slot = (SLOT_PCI << 16) | (busIndex << 8) | s->pci_dev;
                            break;
                        case PCI_CARD_NORTHBRIDGE:
                            add(Node::DEVICE, parent, QCoreApplication::translate("SettingsCards", "Host bridge (device %1)").arg(dev), QString(), s->pci_type);
                            break;
                        case PCI_CARD_SOUTHBRIDGE: {
                            const bool eisa = (s->bus != nullptr) && (s->bus->kind == SLOT_EISA);
                            const int  item = add(Node::BRIDGE, parent, eisa ? QCoreApplication::translate("SettingsCards", "PCI-EISA bridge (device %1)").arg(dev)
                                                                             : QCoreApplication::translate("SettingsCards", "PCI-ISA bridge (device %1)").arg(dev));
                            if (s->bus != nullptr)
                                walk(item, s->bus, busIndex);
                            break;
                        }
                        case PCI_CARD_AGPBRIDGE: {
                            const int item = add(Node::BRIDGE, parent, QCoreApplication::translate("SettingsCards", "AGP bridge (device %1)").arg(dev));
                            const int behind = nextBus++;
                            if (s->bus != nullptr)
                                walk(item, s->bus, behind);
                            break;
                        }
                        case PCI_CARD_BRIDGE:
                            /* A board's own PCI-to-PCI bridge, its slots
                               declared (the OptiPlex riser's 21152); one
                               without is just a device. */
                            if (s->bus == nullptr) {
                                add(Node::DEVICE, parent, QCoreApplication::translate("SettingsCards", "PCI bridge (device %1)").arg(dev), QString(), s->pci_type);
                                break;
                            }
                            [[fallthrough]];
                        case PCI_CARD_NORTHBRIDGE_AUX: {
                            /* A bus of its own behind the bridge: the 450GX's
                               auxiliary host bridge, or a PCI-to-PCI one. */
                            const int item   = add(Node::BRIDGE, parent, (s->pci_type == PCI_CARD_BRIDGE) ?
                                                   QCoreApplication::translate("SettingsCards", "PCI-to-PCI bridge (device %1)").arg(dev) :
                                                   QCoreApplication::translate("SettingsCards", "Second host bridge (device %1)").arg(dev));
                            const int behind = nextBus++;
                            if (s->bus != nullptr) {
                                const int busNode = add(Node::BUS, item, QCoreApplication::translate("SettingsCards", "PCI bus %1").arg(behind));
                                walk(busNode, s->bus, behind);
                            }
                            break;
                        }
                        case PCI_CARD_SOUTHBRIDGE_IDE:
                        case PCI_CARD_SOUTHBRIDGE_PMU:
                        case PCI_CARD_SOUTHBRIDGE_USB:
                        case PCI_CARD_NORTHBRIDGE_SEC:
                            break;
                        default: {
                            static const QMap<int, const char *> kinds = {
                                { PCI_CARD_VIDEO, QT_TRANSLATE_NOOP("SettingsCards", "On-board video") },
                                { PCI_CARD_SOUND, QT_TRANSLATE_NOOP("SettingsCards", "On-board sound") },
                                { PCI_CARD_SCSI, QT_TRANSLATE_NOOP("SettingsCards", "On-board SCSI") },
                                { PCI_CARD_IDE, QT_TRANSLATE_NOOP("SettingsCards", "On-board IDE") },
                                { PCI_CARD_NETWORK, QT_TRANSLATE_NOOP("SettingsCards", "On-board network") },
                                { PCI_CARD_USB, QT_TRANSLATE_NOOP("SettingsCards", "On-board USB") },
                                { PCI_CARD_BRIDGE, QT_TRANSLATE_NOOP("SettingsCards", "PCI bridge") },
                                { PCI_CARD_AGP, QT_TRANSLATE_NOOP("SettingsCards", "AGP slot") },
                            };
                            add(Node::DEVICE, parent,
                                QCoreApplication::translate("SettingsCards", "%1 (device %2)")
                                    .arg(QCoreApplication::translate("SettingsCards", kinds.value(s->pci_type, QT_TRANSLATE_NOOP("SettingsCards", "On-board device"))))
                                    .arg(dev),
                                QString(), s->pci_type);
                            break;
                        }
                    }
                    break;
                case SLOT_AGP:
                    slotsOf(parent, "AGP", s->count);
                    break;
                case SLOT_ISA8:
                    slotsOf(parent, "ISA8", s->count);
                    break;
                case SLOT_ISA16:
                    slotsOf(parent, "ISA", s->count);
                    break;
                case SLOT_ISA_VLB:
                    slotsOf(parent, "ISA/VLB", s->count);
                    break;
                case SLOT_VLB:
                    slotsOf(parent, "VLB", s->count);
                    break;
                case SLOT_EISA:
                    slotsOf(parent, "EISA", s->count);
                    break;
                case SLOT_MCA:
                    slotsOf(parent, "MCA", s->count);
                    break;
                case SLOT_MCA_INTERNAL: {
                    /* Numbered on from the expansion slots, as the board
                       numbers them, and said to be the internal one. */
                    const int first = nodes.size();
                    slotsOf(parent, "MCA", s->count);
                    for (int n = first; n < nodes.size(); n++)
                        nodes[n].label += QCoreApplication::translate("SettingsCards", " (internal fixed disk)");
                    break;
                }
                default:
                    break;
            }
        }
    };

    int root = -1;
    if ((m->slot_layout != nullptr) && (m->slot_layout->kind == SLOT_PCI)) {
        root = add(Node::BUS, -1, QCoreApplication::translate("SettingsCards", "PCI bus 0"));
        walk(root, m->slot_layout, 0);
    } else if (m->slot_layout != nullptr) {
        int mcaBus = -1;
        for (const machine_slot_t *s = m->slot_layout; s->kind != SLOT_END; s++) {
            static const QMap<int, QString> names = { { SLOT_ISA8, "ISA" }, { SLOT_ISA16, "ISA" }, { SLOT_ISA_VLB, "ISA/VLB" }, { SLOT_VLB, "VLB" },
                                                      { SLOT_EISA, "EISA" }, { SLOT_MCA, "MCA" }, { SLOT_AGP, "AGP" } };
            const machine_slot_t one[2] = { *s, MSLOT_END };
            /* The internal MCA slots are on the MCA bus with the others. */
            if ((s->kind == SLOT_MCA_INTERNAL) && (mcaBus >= 0)) {
                walk(mcaBus, one, 0);
                continue;
            }
            const int busNode = add(Node::BUS, -1, QCoreApplication::translate("SettingsCards", "%1 bus").arg(names.value(s->kind == SLOT_MCA_INTERNAL ? SLOT_MCA : s->kind)));
            if (s->kind == SLOT_MCA)
                mcaBus = busNode;
            walk(busNode, one, 0);
        }
    } else {
        static const QStringList groups = { "AGP", "PCI", "VLB", "EISA", "MCA", "ISA" };
        for (const auto &group : groups) {
            if (machineHasGroup(mach, group))
                slotsOf(add(Node::BUS, -1, QCoreApplication::translate("SettingsCards", "%1 bus").arg(group)), group, 0);
        }
    }

    // Plan expansion before placing cards. Each bridge occupies the last
    // position on its parent bus and exposes nine numbered child positions.
    int pciCards = 0;
    int firstBus = 1;
    int registered = 0;
    int physical = 0;
    int lastSlot = -1;
    if (m->slot_layout) {
        std::function<void(const machine_slot_t *)> count = [&](const machine_slot_t *bus) {
            for (auto *s = bus; s->kind != SLOT_END; ++s) {
                if (s->kind != SLOT_PCI)
                    continue;
                registered++;
                if (boardBusBridge(s))
                    firstBus++;
                if ((s->bus != nullptr) && ((s->pci_type == PCI_CARD_NORTHBRIDGE_AUX) || (s->pci_type == PCI_CARD_BRIDGE)))
                    count(s->bus);
            }
        };
        count(m->slot_layout);
    }

    /* The bridges put in by hand, in their order, as pci_register_cards()
       puts them in: each in its slot on one of the board's buses, the bus
       behind it numbered next; the ones 86Box adds follow them. */
    int manual = 0;
    if (!(m->pci_flags & FLAG_NO_BRIDGES)) {
        for (const int slot : bridges) {
            int at = -1;
            for (int n = 0; (n < nodes.size()) && (at < 0); ++n) {
                if ((nodes[n].kind == Node::SLOT) && (nodes[n].slot == slot) && (((slot >> 8) & 0xff) < firstBus))
                    at = n;
            }
            if (at < 0)
                continue;
            const int bus = firstBus + manual;
            manual++;
            nodes[at].kind = Node::BRIDGE;
            nodes[at].label += QCoreApplication::translate("SettingsCards", " — PCI-to-PCI bridge");
            nodes[at].pciType = PCI_CARD_BRIDGE;
            nodes[at].bridge = manual;
            for (int d = 0; d < 9; ++d) {
                const int child = add(Node::SLOT, at, QCoreApplication::translate("SettingsCards", "PCI Slot %1").arg(d + 1), "PCI");
                nodes[child].slot = (SLOT_PCI << 16) | (bus << 8) | d;
            }
            registered += 9;
        }
    }
    firstBus += manual;
    for (int n = 0; n < nodes.size(); ++n) {
        if (nodes[n].kind == Node::SLOT && (nodes[n].slot >> 16) == SLOT_PCI) {
            physical++;
            lastSlot = n;
        }
    }
    int minimum = 0;
    for (const auto &card : cards) {
        if (!card.onboard && card.bus == "PCI") {
            pciCards++;
            const int slot = placement(card.dev, card.instance);
            if ((slot >> 16) == SLOT_PCI)
                minimum = qMax(minimum, ((slot >> 8) & 0xff) - firstBus + 1);
        }
    }
    const int bridges = (m->pci_flags & FLAG_NO_BRIDGES) ? 0 : pci_expansion_bridges(physical, pciCards, minimum, registered);
    for (int b = 0; b < bridges && lastSlot >= 0; ++b) {
        const int parentSlot = nodes[lastSlot].slot;
        const int bus = firstBus + b;
        const int bridge = lastSlot;
        nodes[bridge].kind = Node::BRIDGE;
        nodes[bridge].label += QCoreApplication::translate("SettingsCards", " — PCI-to-PCI bridge");
        nodes[bridge].slot = -1;
        nodes[bridge].pciType = PCI_CARD_BRIDGE;
        int free = -1;
        for (int d = 0; d < 9 && free < 0; ++d) {
            const int candidate = (SLOT_PCI << 16) | (bus << 8) | d;
            bool used = false;
            for (const auto &card : cards)
                used |= placement(card.dev, card.instance) == candidate;
            if (!used)
                free = candidate;
        }
        for (const auto &card : cards)
            if (placement(card.dev, card.instance) == parentSlot)
                assign(card, free);
        for (int d = 0; d < 9; ++d) {
            lastSlot = add(Node::SLOT, bridge, QCoreApplication::translate("SettingsCards", "PCI Slot %1").arg(d + 1), "PCI");
            nodes[lastSlot].slot = (SLOT_PCI << 16) | (bus << 8) | d;
        }
    }

    /* An expansion unit's slots, under the unit; the unit goes under the
       slot its extender card is in once the cards are placed. */
    int unitNode = -1;
    int unitCard = -1;
    for (int i = 0; i < cards.size(); ++i) {
        if ((cards[i].category == CATEGORY_EXPANSION) && (isa_expansion_slots(cards[i].dev) > 0))
            unitCard = i;
    }
    const device_t *unit = (unitCard >= 0) ? cards[unitCard].dev : machineExpansionUnit(mach);
    if (unit != nullptr) {
        const int kind    = isa_expansion_slot_kind(unit);
        unitNode          = add(Node::BRIDGE, root, DeviceConfig::DeviceName(unit, unit->internal_name, 0));
        for (int k = 1; k <= isa_expansion_slots(unit); k++) {
            QString label = QCoreApplication::translate("SettingsCards", "Expansion Slot %1").arg(k);
            if (kind == SLOT_ISA8)
                label += QCoreApplication::translate("SettingsCards", " (8-bit)");
            const int s  = add(Node::SLOT, unitNode, label, (kind == SLOT_ISA8) ? "ISA8" : "ISA");
            nodes[s].slot = (kind << 16) | 0x100 | k;
        }
    }
    /* The extender card goes in the system unit, not in a slot of its own. */
    auto inUnit = [&](const Node &node, int card) {
        return (card == unitCard) && (node.parent == unitNode) && (unitNode >= 0);
    };

    // Explicit placements are resolved before automatic cards can occupy them.
    for (auto &node : nodes) {
        if (node.kind != Node::SLOT || node.slot < 0)
            continue;
        for (int i = 0; i < cards.size(); ++i) {
            if (!placed[i] && !cards[i].onboard && cards[i].dev && fitsDevice(node.bus, cards[i].dev) && !inUnit(node, i) &&
                placement(cards[i].dev, cards[i].instance) == node.slot) {
                node.card = i;
                placed[i] = true;
                break;
            }
        }
    }
    for (auto &node : nodes) {
        if (node.kind != Node::SLOT || node.slot < 0 || node.card >= 0)
            continue;
        for (int i = 0; i < cards.size(); ++i) {
            if (!placed[i] && !cards[i].onboard && cards[i].dev && fitsDevice(node.bus, cards[i].dev) && !inUnit(node, i) &&
                placement(cards[i].dev, cards[i].instance) < 0) {
                node.card = i;
                placed[i] = true;
                break;
            }
        }
    }

    /* The unit under its extender card's slot (a machine's own 5161 has no
       card in the list). */
    if ((unitNode >= 0) && (unitCard >= 0)) {
        for (int n = 0; n < nodes.size(); ++n) {
            if ((nodes[n].card == unitCard) && (n < unitNode))
                nodes[unitNode].parent = n;
        }
    }

    /* The machine's own cards on its on-board devices. */
    for (int i = 0; i < cards.size(); i++) {
        if (!cards[i].onboard)
            continue;

        static const QMap<int, int> types = { { CATEGORY_VIDEO, PCI_CARD_VIDEO }, { CATEGORY_SOUND, PCI_CARD_SOUND },
                                              { CATEGORY_NETWORK, PCI_CARD_NETWORK }, { CATEGORY_HDC, PCI_CARD_IDE } };
        int type = types.value(cards[i].category, -1);
        /* A board without IDE whose own disk controller is its SCSI chip. */
        if ((cards[i].category == CATEGORY_HDC) && !machine_has_flags(mach, MACHINE_IDE_QUAD | MACHINE_IDE) &&
            machine_has_flags(mach, MACHINE_SCSI))
            type = PCI_CARD_SCSI;
        for (auto &node : nodes) {
            if ((node.kind == Node::DEVICE) && (type >= 0) && (node.pciType == type) && (node.card < 0)) {
                node.card = i;
                placed[i] = true;
                break;
            }
        }
    }

    /* PCI cards past the board's slots go behind the bridge 86Box adds on
       PCI bus 0 for them; anything else left has no slot on this board. */
    int bridge = -1;
    int none   = -1;
    int n      = 0;
    for (int i = 0; i < cards.size(); i++) {
        if (placed[i] || cards[i].onboard)
            continue;

        if (physical == 0 && cards[i].bus == "PCI" && placement(cards[i].dev, cards[i].instance) < 0) {
            if (bridge < 0)
                bridge = add(Node::BRIDGE, root, QCoreApplication::translate("SettingsCards", "PCI-PCI bridge (added by 86Box when the slots run out)"));
            nodes[add(Node::SLOT, bridge, QCoreApplication::translate("SettingsCards", "PCI slot %1").arg(++n), "PCI")].card = i;
        } else {
            if (none < 0)
                none = add(Node::BUS, -1, QCoreApplication::translate("SettingsCards", "No slot for these on this board"));
            nodes[add(Node::SLOT, none, cards[i].bus, cards[i].bus)].card = i;
        }
        placed[i] = true;
    }

    return nodes;
}

/* The first bus number behind no bridge of the board's own: the buses
   behind bridges put in by hand start there. */
static int
boardBuses(const machine_t *m)
{
    int first = 1;
    for (auto *s = m->slot_layout; s && (s->kind != SLOT_END); ++s) {
        if (boardBusBridge(s))
            first++;
    }
    return first;
}

/* Every card pinned where it is now, then the placements on PCI buses
   from a number on moved by delta; those on the bus gone (when given)
   dropped. */
static void
renumberBuses(int from, int delta, int gone)
{
    const auto cards = list();
    for (const auto &node : layout(cards)) {
        if ((node.card >= 0) && (node.slot >= 0))
            assign(cards[node.card], node.slot);
    }
    for (auto it = assignments.begin(); it != assignments.end();) {
        const int slot = it.value();
        const int bus  = (slot >> 8) & 0xff;
        if (((slot >> 16) == SLOT_PCI) && (bus == gone))
            it = assignments.erase(it);
        else {
            if (((slot >> 16) == SLOT_PCI) && (bus >= from))
                it.value() = (slot & ~0xff00) | ((bus + delta) << 8);
            ++it;
        }
    }
    assignmentsEdited = true;
}

bool
canAddBridge(int slot, bool occupied)
{
    const machine_t *m = &machines[Settings::settings->currentMachine()];

    if ((m->slot_layout == nullptr) || (m->slot_layout->kind != SLOT_PCI) || (m->pci_flags & FLAG_NO_BRIDGES) ||
        ((slot >> 16) != SLOT_PCI) || (((slot >> 8) & 0xff) >= boardBuses(m)) || bridges.contains(slot))
        return false;

    /* An empty slot: no card in it, unless it may move. */
    const auto cards = list();
    for (const auto &node : layout(cards)) {
        if ((node.kind == Node::SLOT) && (node.slot == slot))
            return occupied || (node.card < 0);
    }
    return false;
}

void
addBridge(int slot)
{
    if (!canAddBridge(slot, true))
        return;

    /* Its bus is numbered after the other hand-put bridges', before those
       86Box adds: theirs move up one. A card in its slot goes to the first
       slot behind it. */
    const machine_t *m     = &machines[Settings::settings->currentMachine()];
    const auto       cards = list();
    int              made  = 0;
    int              moved = -1;
    for (const auto &node : layout(cards)) {
        made += (node.bridge > 0);
        if ((node.kind == Node::SLOT) && (node.slot == slot))
            moved = node.card;
    }
    renumberBuses(boardBuses(m) + made, 1, -1);
    bridges.append(slot);
    if (moved >= 0)
        assign(cards[moved], (SLOT_PCI << 16) | ((boardBuses(m) + made) << 8));
    layoutChanged();
}

void
removeBridge(int n)
{
    const machine_t *m    = &machines[Settings::settings->currentMachine()];
    int              slot = -1;
    for (const auto &node : layout(list())) {
        if (node.bridge == n)
            slot = node.slot;
    }
    if (slot < 0)
        return;

    /* The cards behind it go where 86Box puts them; the buses after its
       move down one. */
    const int bus = boardBuses(m) + n - 1;
    renumberBuses(bus + 1, -1, bus);
    bridges.removeAll(slot);
    layoutChanged();
}

/* The last empty slot of the board's own PCI buses that takes a bridge,
   where 86Box puts the bridges it adds; -1 where there is none. */
int
bridgeSlot()
{
    int empty = -1;
    int any   = -1;
    for (const auto &node : layout(list())) {
        if ((node.kind == Node::SLOT) && canAddBridge(node.slot, true)) {
            any = node.slot;
            if (node.card < 0)
                empty = node.slot;
        }
    }
    return (empty >= 0) ? empty : any;
}

/* Where a card is: its slot, and the bridge that slot is behind. */
QString
location(const QList<Node> &nodes, int card)
{
    for (const auto &node : nodes) {
        if (node.card != card)
            continue;

        if ((node.kind == Node::SLOT) && (node.parent >= 0) && (nodes[node.parent].kind == Node::BRIDGE) &&
            (nodes[node.parent].pciType == PCI_CARD_BRIDGE || nodes[node.parent].label.startsWith("PCI-PCI")))
            return QCoreApplication::translate("SettingsCards", "%1 (bridge bus %2)").arg(node.label).arg((node.slot >> 8) & 0xff);

        return node.label;
    }

    return QCoreApplication::translate("SettingsCards", "On the board");
}
}

namespace Borrow {

/* Where a widget was: in a tab, in a stack, or in a layout's cell. */
struct Place {
    QPointer<QWidget> parent;
    QPointer<QWidget> container;
    int               index;
    QString           title;
    QIcon             icon;
    int               row;
    int               column;
    int               rowSpan;
    int               columnSpan;
    QString           layout;
};

/* A widget can be borrowed again by another view: each borrow goes back
   to where the one before left it. */
static QHash<QWidget *, QList<Place>> places;

static bool
findInLayout(QLayout *layout, QWidget *widget, QLayout *&owner, int &index)
{
    if (layout == nullptr)
        return false;

    for (int i = 0; i < layout->count(); i++) {
        auto *item = layout->itemAt(i);
        if (item->widget() == widget) {
            owner = layout;
            index = i;
            return true;
        }
        if ((item->layout() != nullptr) && findInLayout(item->layout(), widget, owner, index))
            return true;
    }

    return false;
}

void
take(QWidget *widget, QWidget *into)
{
    if (widget == nullptr)
        return;

    Place place {};
    place.parent = widget->parentWidget();

    if (auto *tabs = (place.parent != nullptr) ? qobject_cast<QTabWidget *>(place.parent->parentWidget()) : nullptr;
        (tabs != nullptr) && (tabs->indexOf(widget) >= 0)) {
        place.container = tabs;
        place.index     = tabs->indexOf(widget);
        place.title     = tabs->tabText(place.index);
        place.icon      = tabs->tabIcon(place.index);
        tabs->removeTab(place.index);
    } else if (auto *stack = qobject_cast<QStackedWidget *>(place.parent)) {
        place.container = stack;
        place.index     = stack->indexOf(widget);
        stack->removeWidget(widget);
    } else if (place.parent != nullptr) {
        QLayout *owner = nullptr;
        int      index = -1;
        if (findInLayout(place.parent->layout(), widget, owner, index)) {
            place.index = index;
            if (auto *grid = qobject_cast<QGridLayout *>(owner))
                grid->getItemPosition(index, &place.row, &place.column, &place.rowSpan, &place.columnSpan);
            else
                place.row = -1;
            place.container = nullptr;
            owner->removeWidget(widget);
            /* The layout, kept by name on the parent, to go back into. */
            if (owner->objectName().isEmpty())
                owner->setObjectName(QString("borrowLayout%1").arg(reinterpret_cast<quintptr>(owner)));
            place.layout = owner->objectName();
        }
    }

    places[widget].append(place);
    widget->setParent(into);
}

static QLayout *
layoutNamed(QWidget *parent, const QString &name)
{
    if (parent == nullptr)
        return nullptr;
    if ((parent->layout() != nullptr) && (parent->layout()->objectName() == name))
        return parent->layout();

    return parent->findChild<QLayout *>(name);
}

void
giveBack(QWidget *widget)
{
    if (!places.contains(widget) || places[widget].isEmpty())
        return;

    const Place place = places[widget].takeLast();
    if (places[widget].isEmpty())
        places.remove(widget);

    /* A tab or a page of a stack is shown by its tab widget or stack, when
       it is the current one: showing it here would put it over that one. */
    if (auto *tabs = qobject_cast<QTabWidget *>(place.container.data())) {
        tabs->insertTab(place.index, widget, place.icon, place.title);
        return;
    } else if (auto *stack = qobject_cast<QStackedWidget *>(place.container.data())) {
        QWidget *current = stack->currentWidget();
        stack->insertWidget(place.index, widget);
        if (current != nullptr)
            stack->setCurrentWidget(current);
        widget->setVisible(stack->currentWidget() == widget);
        return;
    } else if (place.parent != nullptr) {
        auto *owner = layoutNamed(place.parent, place.layout);
        widget->setParent(place.parent);
        if (auto *grid = qobject_cast<QGridLayout *>(owner))
            grid->addWidget(widget, place.row, place.column, place.rowSpan, place.columnSpan);
        else if (auto *box = qobject_cast<QBoxLayout *>(owner))
            box->insertWidget(qMin(place.index, box->count()), widget);
        else if (owner != nullptr)
            owner->addWidget(widget);
    } else {
        /* It had no parent (or its parent is gone): none again, and hidden.
           Left in the window it was lent to, that window would delete it
           with itself, and its owner -- often a widget on the stack --
           would delete it again. */
        widget->hide();
        widget->setParent(nullptr);
        return;
    }
    widget->show();
}

void
giveBackAll()
{
    while (!places.isEmpty()) {
        const auto widgets = places.keys();
        for (auto *widget : widgets)
            giveBack(widget);
    }
}

/* A control's row on a classic page hidden: its label, the control and
   its Configure button, whatever layout holds them. */
void
hideRow(QWidget *widget)
{
    if ((widget == nullptr) || (widget->parentWidget() == nullptr))
        return;

    QLayout *owner = nullptr;
    int      index = -1;
    if (!findInLayout(widget->parentWidget()->layout(), widget, owner, index)) {
        widget->hide();
        return;
    }

    if (auto *grid = qobject_cast<QGridLayout *>(owner)) {
        int row;
        int column;
        int rowSpan;
        int columnSpan;
        grid->getItemPosition(index, &row, &column, &rowSpan, &columnSpan);
        for (int c = 0; c < grid->columnCount(); c++) {
            if (auto *item = grid->itemAtPosition(row, c)) {
                if (item->widget() != nullptr)
                    item->widget()->hide();
                else if (item->layout() != nullptr) {
                    for (int i = 0; i < item->layout()->count(); i++) {
                        if (auto *w = item->layout()->itemAt(i)->widget())
                            w->hide();
                    }
                }
            }
        }
    } else if (auto *form = qobject_cast<QFormLayout *>(owner)) {
        if (auto *label = form->labelForField(widget))
            label->hide();
        widget->hide();
    } else {
        /* A row of its own, as a check box and its button are. */
        for (int i = 0; i < owner->count(); i++) {
            if (auto *w = owner->itemAt(i)->widget())
                w->hide();
        }
    }
}
}

/* Follow the classic pages' card controls, so a view shows what they
   hold whatever changes them. */
static void
followCards(QWidget *view, const std::function<void()> &refresh)
{
    Cards::follow(view, refresh);
    for (auto *cbox : Settings::settings->findChildren<QComboBox *>()) {
        const QString name = cbox->objectName();
        static const QStringList prefixes = { "comboBoxVideo", "comboBoxSoundCard", "comboBoxNIC", "comboBoxHD", "comboBoxSCSI", "comboBoxFD",
                                              "comboBoxCDInterface", "comboBoxMemExpCard", "comboBoxIsaRomCard", "comboBoxRTC", "comboBoxIsaExpansion",
                                              "comboBoxGameportCard", "comboBoxMachine" };
        for (const auto &prefix : prefixes) {
            if (name.startsWith(prefix)) {
                QObject::connect(cbox, QOverload<int>::of(&QComboBox::currentIndexChanged), view, [refresh]() { refresh(); });
                break;
            }
        }
    }
    static const QStringList checks = { "checkBoxVoodoo", "checkBox8514", "checkBoxXga", "checkBoxDa2", "checkBoxISABugger",
                                        "checkBoxPOSTCard", "checkBoxUnitTester", "checkBoxKeyCard", "checkBoxSoftPower",
                                        "checkBoxSerial1", "checkBoxSerial2", "checkBoxSerial3", "checkBoxSerial4",
                                        "checkBoxParallel1", "checkBoxParallel2", "checkBoxParallel3", "checkBoxParallel4" };
    for (const auto &name : checks) {
        if (auto *cbox = qobject_cast<QCheckBox *>(Cards::control(name)))
            QObject::connect(cbox, &QCheckBox::toggled, view, [refresh]() { refresh(); });
    }
}

SettingsCards::SettingsCards(QWidget *parent)
    : QWidget(parent)
{
    auto *layout = new QVBoxLayout(this);
    auto *top    = new QHBoxLayout();

    buttonAdd = new QPushButton(tr("Add card"), this);
    top->addWidget(buttonAdd);
    top->addStretch();

    tree = new QTreeWidget(this);
    tree->setColumnCount(3);
    tree->setSelectionMode(QAbstractItemView::ExtendedSelection);
    tree->setRootIsDecorated(false);
    tree->setHeaderLabels({ tr("Installed card"), tr("Location"), tr("Actions") });
    tree->setAlternatingRowColors(true);
    tree->header()->setStretchLastSection(false);
    tree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    tree->setColumnWidth(1, 150);
    tree->setColumnWidth(2, 195);

    layout->addLayout(top);
    layout->addWidget(tree);
    auto *bottom = new QHBoxLayout();
    buttonRemove = new QPushButton(tr("Remove"), this);
    buttonRemove->setEnabled(false);
    bottom->addWidget(buttonRemove);
    auto *hint = new QLabel(tr("Use Ctrl or Shift to select several cards."), this);
    bottom->addWidget(hint);
    bottom->addStretch();
    layout->addLayout(bottom);
    connect(tree, &QTreeWidget::itemSelectionChanged, this, [this]() {
        buttonRemove->setEnabled(!tree->selectedItems().isEmpty());
    });
    connect(buttonRemove, &QPushButton::clicked, this, [this]() {
        QList<Cards::Card> selected;
        QList<int>         bridgesOut;
        const auto cards = Cards::list();
        for (auto *item : tree->selectedItems()) {
            const QString key = item->data(0, Qt::UserRole).toString();
            if (key.startsWith("bridge:"))
                bridgesOut.append(key.mid(7).toInt());
            for (const auto &card : cards) {
                if (card.control != item->data(0, Qt::UserRole).toString())
                    continue;
                if (!Cards::removable(card)) {
                    QMessageBox::information(this, tr("Cannot remove selection"),
                        card.onboard ? tr("Can't remove onboard device %1. Nothing was removed.").arg(card.name)
                                     : tr("Can't remove %1. Replace this device instead. Nothing was removed.").arg(card.name));
                    return;
                }
                selected.append(card);
            }
        }
        removing = true;
        for (const auto &card : selected)
            Cards::remove(card);
        /* The highest first: taking one out renumbers those after it. */
        std::sort(bridgesOut.begin(), bridgesOut.end(), std::greater<int>());
        for (const int n : bridgesOut)
            Cards::removeBridge(n);
        removing = false;
        refresh();
    });

    connect(buttonAdd, &QPushButton::clicked, this, [this]() { Cards::pickCard(-1, QString(), buttonAdd); });

    followCards(this, [this]() { refresh(); });
}

void
SettingsCards::showEvent(QShowEvent *event)
{
    refresh();
    QWidget::showEvent(event);
}

/* A row's Configure and Remove, beside the card. */
void
SettingsCards::addCardRow(QTreeWidgetItem *item, const Cards::Card &card, const QString &location)
{
    item->setText(0, card.name);
    item->setText(1, location);
    item->setData(0, Qt::UserRole, card.control);
    item->setToolTip(0, Cards::categoryTitle(card.category));

    auto *actions = new QWidget(tree);
    auto *row     = new QHBoxLayout(actions);
    row->setContentsMargins(0, 0, 0, 0);
    auto *conf = new QToolButton(actions);
    conf->setText(tr("Configure"));
    conf->setEnabled(Cards::configurable(card));
    connect(conf, &QToolButton::clicked, this, [card]() { Cards::configure(card); });
    row->addWidget(conf);
    if (!card.onboard) {
        auto *replace = new QToolButton(actions);
        replace->setText(tr("Replace..."));
        connect(replace, &QToolButton::clicked, this, [this, card, replace]() {
            QList<Cards::PickItem> items;
            const auto nodes = Cards::layout(Cards::list());
            QString bus = card.bus;
            const int slot = Cards::placement(card.dev, card.instance);
            for (const auto &node : nodes)
                if (node.slot == slot && slot >= 0)
                    bus = node.bus;
            for (const auto &choice : Cards::choices(-1, bus, &card))
                items.append({ { Cards::categoryTitle(choice.category) }, choice.name,
                               [this, card, choice, slot]() { Cards::put(choice, &card, this, slot); refresh(); } });
            Cards::pickFrom(items, replace);
        });
        row->addWidget(replace);
    }
    row->addStretch();
    tree->setItemWidget(item, 2, actions);
}

/* Every card as one list: the machine's own first, then ISA cards, then
   EISA, then the rest, each alphabetical. The floppy controller is always
   there, to be chosen. */
void
SettingsCards::refresh()
{
    if (removing)
        return;
    if (!isVisible() && (tree->topLevelItemCount() > 0))
        return;

    Cards::nameOnboardEntries();
    QList<Cards::Card> sorted = Cards::list();

    const auto nodes = Cards::layout(sorted);
    QHash<QString, int> locationOrder;
    QHash<QString, QString> locations;
    QHash<QString, QString> buses;
    for (int n = 0; n < nodes.size(); ++n) {
        if (nodes[n].card < 0)
            continue;
        const auto &card = sorted[nodes[n].card];
        locationOrder.insert(card.control, n);
        buses.insert(card.control, nodes[n].bus);
        locations.insert(card.control, Cards::location(nodes, nodes[n].card));
    }
    auto rank = [&buses](const Cards::Card &card) {
        if (card.onboard)
            return -1;
        const QString bus = buses.value(card.control, card.bus);
        if (bus == "ISA")
            return 0;
        if (bus == "EISA")
            return 1;
        return 2;
    };
    std::sort(sorted.begin(), sorted.end(), [&rank, &locationOrder](const Cards::Card &a, const Cards::Card &b) {
        if (rank(a) != rank(b))
            return rank(a) < rank(b);
        const int ap = locationOrder.value(a.control, 10000);
        const int bp = locationOrder.value(b.control, 10000);
        if (!a.onboard && ap != bp)
            return ap < bp;
        return QString::localeAwareCompare(a.name, b.name) < 0;
    });

    QSet<QString> selected;
    for (auto *item : tree->selectedItems())
        selected.insert(item->data(0, Qt::UserRole).toString());
    const int scroll = tree->verticalScrollBar()->value();
    QSignalBlocker blocked(tree);
    tree->clear();
    bool floppy = false;
    for (const auto &card : sorted) {
        addCardRow(new QTreeWidgetItem(tree), card, card.onboard ? tr("Onboard") : locations.value(card.control));
        floppy |= (card.category == Cards::CATEGORY_FDC);
    }

    /* The bridges put in by hand, where they are. */
    for (const auto &node : nodes) {
        if (node.bridge <= 0)
            continue;
        QString where = node.label;
        where.chop(QCoreApplication::translate("SettingsCards", " — PCI-to-PCI bridge").size());
        auto *item = new QTreeWidgetItem(tree, { DeviceConfig::DeviceName(&dec21150_device, dec21150_device.internal_name, 0), where });
        item->setData(0, Qt::UserRole, QString("bridge:%1").arg(node.bridge));
        item->setToolTip(0, tr("PCI-to-PCI bridge, with nine PCI slots behind it"));
    }

    if (!floppy) {
        auto *item = new QTreeWidgetItem(tree, { tr("(no floppy controller)"), Cards::categoryTitle(Cards::CATEGORY_FDC) });
        item->setForeground(0, palette().brush(QPalette::Disabled, QPalette::Text));
        auto *add = new QToolButton(tree);
        add->setText(tr("Add..."));
        connect(add, &QToolButton::clicked, this, [add]() { Cards::pickCard(Cards::CATEGORY_FDC, QString(), add); });
        tree->setItemWidget(item, 2, add);
    }

    for (int i = 0; i < tree->topLevelItemCount(); ++i)
        if (selected.contains(tree->topLevelItem(i)->data(0, Qt::UserRole).toString()))
            tree->topLevelItem(i)->setSelected(true);
    buttonRemove->setEnabled(!tree->selectedItems().isEmpty());
    tree->verticalScrollBar()->setValue(scroll);
    Cards::prunePanels();
}

SettingsCardPage::SettingsCardPage(int category, QWidget *classic, QWidget *parent)
    : QWidget(parent)
    , category(category)
    , classic(classic)
{
    auto *outer  = new QVBoxLayout(this);
    auto *scroll = new QScrollArea(this);
    auto *inner  = new QWidget(scroll);
    auto *column = new QVBoxLayout(inner);

    outer->setContentsMargins(0, 0, 0, 0);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);

    missingHardware = new QLabel(inner);
    missingHardware->setWordWrap(true);
    missingHardware->setStyleSheet(QStringLiteral("color: #b00020; font-weight: bold; padding: 8px 2px;"));
    column->addWidget(missingHardware);
    blocks = new QVBoxLayout();
    column->addLayout(blocks);

    auto *addRow = new QHBoxLayout();
    buttonAdd    = new QPushButton(tr("Add %1 card...").arg(Cards::categoryTitle(category).toLower()), inner);
    addRow->addWidget(buttonAdd);
    addRow->addStretch();
    column->addLayout(addRow);

    /* The page's own settings, without its card rows. */
    Borrow::take(classic, inner);
    classic->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);
    auto *section = new QLabel(category == Cards::CATEGORY_SOUND ? tr("Audio output and synthesis")
                               : category == Cards::CATEGORY_VIDEO ? tr("Monitor settings")
                               : tr("Network connection"), inner);
    QFont sectionFont = section->font();
    sectionFont.setBold(true);
    section->setFont(sectionFont);
    section->setContentsMargins(2, 12, 0, 3);
    if (category != Cards::CATEGORY_NETWORK)
        column->addWidget(section);
    else
        section->hide();
    column->addWidget(classic);
    classic->show();
    column->addStretch();

    scroll->setWidget(inner);
    outer->addWidget(scroll);

    connect(buttonAdd, &QPushButton::clicked, this, [this]() { Cards::pickCard(this->category, QString(), buttonAdd); });

    followCards(this, [this]() { refresh(); });
}

void
SettingsCardPage::showEvent(QShowEvent *event)
{
    refresh();
    QWidget::showEvent(event);
}

/* A network card's connection settings are its own tab on the classic
   page: it goes with the card. */
QWidget *
SettingsCardPage::extraFor(const Cards::Card &card) const
{
    if (card.category == Cards::CATEGORY_NETWORK)
        return Cards::control(QString("tabNet%1").arg(card.index + 1));

    return nullptr;
}

SettingsCardPage::Block
SettingsCardPage::makeBlock(const Cards::Card &card, const QString &key, const QString &where)
{
    Block block;
    auto *box    = new QGroupBox(Cards::categoryTitle(card.category), this);
    auto *layout = new QVBoxLayout(box);
    auto *head   = new QHBoxLayout();
    auto *title  = new QLabel(QString("<b>%1</b>").arg(card.name.toHtmlEscaped()), box);
    auto *place  = new QLabel(where, box);

    place->setForegroundRole(QPalette::PlaceholderText);
    head->addWidget(title);
    head->addWidget(place);
    head->addStretch();
    if (Cards::removable(card)) {
        auto *rem = new QPushButton(tr("Remove"), box);
        connect(rem, &QPushButton::clicked, this, [card]() { Cards::remove(card); });
        head->addWidget(rem);
    }
    layout->addLayout(head);

    block.key   = key;
    block.box   = box;
    block.panel = Cards::panel(card);
    if (!block.panel.isNull()) {
        layout->addWidget(block.panel);
        block.panel->show();
    }

    block.extra = extraFor(card);
    if (!block.extra.isNull()) {
        Borrow::take(block.extra, box);
        /* Lined up with the card's options above it: no margins of its own,
           and its labels as wide as theirs. Both put back with it
           (dropBlock()). */
        if (auto *grid = qobject_cast<QGridLayout *>(block.extra->layout())) {
            const QMargins margins = grid->contentsMargins();
            block.extra->setProperty("borrowMargins", QRect(margins.left(), margins.top(), margins.right(), margins.bottom()));
            block.extra->setProperty("borrowColumn", grid->columnMinimumWidth(0));
            grid->setContentsMargins(0, 0, 0, 0);
            int width = 0;
            if (auto *form = (block.panel.isNull()) ? nullptr : qobject_cast<QFormLayout *>(block.panel->layout())) {
                for (int row = 0; row < form->rowCount(); row++) {
                    if (auto *item = form->itemAt(row, QFormLayout::LabelRole); (item != nullptr) && (item->widget() != nullptr))
                        width = qMax(width, item->widget()->sizeHint().width());
                }
            }
            if (width > 0)
                grid->setColumnMinimumWidth(0, width);
        }
        block.extra->setMinimumHeight(0);
        block.extra->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);
        layout->addWidget(block.extra);
        block.extra->show();
        for (auto *label : block.extra->findChildren<QLabel *>())
            if (label->text() == tr("Mode:"))
                label->setText(tr("Connection:"));
        for (auto *combo : block.extra->findChildren<QComboBox *>()) {
            const int none = combo->findText(tr("Null Driver"));
            if (none >= 0)
                combo->setItemText(none, tr("Disconnected"));
        }
    }

    if (block.panel.isNull() && block.extra.isNull()) {
        auto *none = new QLabel(tr("This card has no settings."), box);
        none->setForegroundRole(QPalette::PlaceholderText);
        layout->addWidget(none);
    }

    return block;
}

/* A card's box goes, its panel kept for the card and its borrowed
   settings put back. */
void
SettingsCardPage::dropBlock(const Block &block)
{
    Cards::releasePanel(block.panel);
    if (!block.extra.isNull()) {
        if (auto *grid = qobject_cast<QGridLayout *>(block.extra->layout()); (grid != nullptr) && block.extra->property("borrowMargins").isValid()) {
            const QRect margins = block.extra->property("borrowMargins").toRect();
            grid->setContentsMargins(margins.x(), margins.y(), margins.width(), margins.height());
            grid->setColumnMinimumWidth(0, block.extra->property("borrowColumn").toInt());
        }
        Borrow::giveBack(block.extra);
    }
    block.box->deleteLater();
}

void
SettingsCardPage::releaseAll()
{
    for (const auto &block : shown)
        dropBlock(block);
    shown.clear();
}

void
SettingsCardPage::refresh()
{
    if (!isVisible())
        return;

    Cards::nameOnboardEntries();
    const auto list  = Cards::list();
    const auto nodes = Cards::layout(list);

    QList<Block> next;
    for (int i = 0; i < list.size(); i++) {
        const auto &card = list[i];
        if (card.category != category)
            continue;

        const QString key = QString("%1|%2").arg(card.control, card.name);
        const QString where = card.onboard ? tr("On the board") : Cards::location(nodes, i);

        int found = -1;
        for (int j = 0; j < shown.size(); j++) {
            if (shown[j].key == key) {
                found = j;
                break;
            }
        }
        if (found >= 0) {
            /* Where it is can change as other cards come and go. */
            for (auto *label : shown[found].box->findChildren<QLabel *>()) {
                if (label->foregroundRole() == QPalette::PlaceholderText && !label->text().startsWith(tr("This card")))
                    label->setText(where);
            }
            next.append(shown.takeAt(found));
        } else
            next.append(makeBlock(card, key, where));
    }

    for (const auto &block : shown)
        dropBlock(block);
    shown = next;
    missingHardware->setText(category == Cards::CATEGORY_SOUND ? tr("No sound card installed.")
                             : category == Cards::CATEGORY_NETWORK ? tr("No network card installed.")
                             : tr("No video card installed."));
    missingHardware->setVisible(shown.isEmpty());

    for (const auto &block : shown) {
        blocks->removeWidget(block.box);
        blocks->addWidget(block.box);
    }

    buttonAdd->setEnabled(!Cards::choices(category, QString()).isEmpty());
    Cards::prunePanels();
}
