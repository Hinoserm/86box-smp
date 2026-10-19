/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          Machine selection and configuration UI module.
 *
 * Authors: Joakim L. Gilje <jgilje@jgilje.net>
 *
 *          Copyright 2021 Joakim L. Gilje
 */
#include "qt_settings_completer.hpp"

#include "qt_settingsmachine.hpp"
#include "ui_qt_settingsmachine.h"

#include <QDebug>
#include <QDialog>
#include <QFrame>
#include <QVBoxLayout>
#include <QDialogButtonBox>
#include <QStandardItem>
#include <QStandardItemModel>
#include <QCompleter>
#include <QSignalBlocker>
#include <QTimer>

#include <algorithm>

extern "C" {
#include "../cpu/cpu.h"

#include <86box/86box.h>
#include <86box/ini.h>
#include <86box/config.h>
#include <86box/device.h>
#include <86box/machine.h>
#include <86box/nvr.h>
#include <86box/cpu_mp.h>
}

#include "qt_deviceconfig.hpp"
#include "qt_models_common.hpp"

#include "qt_defs.hpp"

/* The FPU emulation dropdown: the default FPU, the host's x87 (on builds that
   have it) and softfloat. The host-x87 FPU runs on softfloat's state, so it
   sets fpu_softfloat underneath as well as fpu_host_x87. */
enum {
    FPU_MODE_DEFAULT   = 0,
    FPU_MODE_HOST_X87  = 1,
    FPU_MODE_SOFTFLOAT = 2
};

static int
fpu_mode_current(void)
{
    if (fpu_host_x87)
        return FPU_MODE_HOST_X87;
    return fpu_softfloat ? FPU_MODE_SOFTFLOAT : FPU_MODE_DEFAULT;
}

SettingsMachine::SettingsMachine(QWidget *parent)
    : QWidget(parent)
    , ui(new Ui::SettingsMachine)
{
    ui->setupUi(this);

    ui->comboBoxFPUMode->addItem(tr("Default"), FPU_MODE_DEFAULT);
#if X87_HOST_BUILD
    ui->comboBoxFPUMode->addItem(tr("Host x87 (exact)"), FPU_MODE_HOST_X87);
#endif
    ui->comboBoxFPUMode->addItem(tr("Softfloat (exact)"), FPU_MODE_SOFTFLOAT);

    machine_cfg_changed             = 0;

    sc                              = new SettingsCompleter(ui->comboBoxMachine, ui->comboBoxMachineType);

    int         selectedMachineType = 0;
    auto       *machineTypesModel   = ui->comboBoxMachineType->model();
    int         i                   = -1;
    int         j                   = 0;
    int         cur_j               = 0;
    const void *miname;
    Models::Batch machineTypeRows(machineTypesModel);
    do {
        miname = machine_get_internal_name_ex(j);

        if ((miname == nullptr) || (machine_get_type(j) != i)) {
            if ((i != -1) && (cur_j != 0)) {
                int row = machineTypeRows.add(tr(machine_types[i].name), machine_types[i].id);
                if (machine_types[i].id == machine_get_type(machine))
                    selectedMachineType = row;
            }

            i     = machine_get_type(j);
            cur_j = 0;
        }

        if (machine_available(j)) {
            sc->addMachine(i, j);

            cur_j++;
        }

        j++;
    } while (miname != nullptr);
    machineTypeRows.commit();

    auto warning_icon = QIcon(":/misc/qt/icons/warning.ico");
    ui->softFloatWarningIcon->setPixmap(warning_icon.pixmap(warning_icon.actualSize(QSize(16, 16))));
    ui->softFloatWarningIcon->setVisible(false);
    ui->softFloatWarningText->setVisible(false);

    auto *waitStatesModel = ui->comboBoxWaitStates->model();
    waitStatesModel->insertRows(0, 9);
    auto idx = waitStatesModel->index(0, 0);
    waitStatesModel->setData(idx, tr("Default"), Qt::DisplayRole);
    waitStatesModel->setData(idx, 0, Qt::UserRole);
    for (int i = 0; i < 8; ++i) {
        idx = waitStatesModel->index(i + 1, 0);
        waitStatesModel->setData(idx, tr("%n wait state(s)", "", i), Qt::DisplayRole);
        waitStatesModel->setData(idx, i + 1, Qt::UserRole);
    }

    auto *pitModeModel = ui->comboBoxPitMode->model();
    pitModeModel->insertRows(0, 3);
    idx = pitModeModel->index(0, 0);
    pitModeModel->setData(idx, tr("Auto"), Qt::DisplayRole);
    pitModeModel->setData(idx, -1, Qt::UserRole);
    idx = pitModeModel->index(1, 0);
    pitModeModel->setData(idx, tr("Slow"), Qt::DisplayRole);
    pitModeModel->setData(idx, 0, Qt::UserRole);
    idx = pitModeModel->index(2, 0);
    pitModeModel->setData(idx, tr("Fast"), Qt::DisplayRole);
    pitModeModel->setData(idx, 1, Qt::UserRole);

    ui->comboBoxPitMode->setCurrentIndex(-1);
    ui->comboBoxPitMode->setCurrentIndex(pit_mode + 1);

    /* Processors fitted and SMP drift: shown for boards with more than one
       socket (on_comboBoxMachine_currentIndexChanged), one entry for each
       socket (updateProcessorsPairable). */
    wantedProcessors = (cpu_count > 1) ? cpu_count : 1;
    {
        static const int drifts[] = { 5, 10, 20, 50, 100 };
        int              sel      = -1;

        for (int d : drifts) {
            int row = Models::AddEntry(ui->comboBoxSmpDrift->model(), tr("%1 µs").arg(d), d);
            if (d == cpu_smp_drift_us)
                sel = row;
        }
        if (sel < 0) /* set by hand in the file: keep it */
            sel = Models::AddEntry(ui->comboBoxSmpDrift->model(), tr("%1 µs").arg(cpu_smp_drift_us), cpu_smp_drift_us);
        ui->comboBoxSmpDrift->setCurrentIndex(sel);
    }

    switch (time_sync) {
        case TIME_SYNC_ENABLED:
            ui->radioButtonLocalTime->setChecked(true);
            break;
        case TIME_SYNC_ENABLED | TIME_SYNC_UTC:
            ui->radioButtonUTC->setChecked(true);
            break;
        case TIME_SYNC_DISABLED:
        default:
            ui->radioButtonDisabled->setChecked(true);
            break;
    }

    ui->comboBoxMachineType->setCurrentIndex(-1);
    ui->comboBoxMachineType->setCurrentIndex(selectedMachineType);

    ui->radioButtonLargerFrames->setChecked(force_10ms);
    ui->radioButtonSmallerFrames->setChecked(!force_10ms);

    ui->checkBoxOverrideInterpreter->setChecked(cpu_override_interpreter ? true : false);

#ifndef USE_DYNAREC
    ui->checkBoxDynamicRecompiler->setEnabled(false);
    ui->checkBoxDynamicRecompiler->setVisible(false);
#endif
}

SettingsMachine::~SettingsMachine()
{
    delete sc;

    delete ui;
}

int
SettingsMachine::changed()
{
    int has_changed = 0;
    int temp;

    has_changed |= (machine                    != ui->comboBoxMachine->currentData().toInt());
    has_changed |= machine_cfg_changed;
    has_changed |= (cpu_f                      != const_cast<cpu_family_t *>(&cpu_families[ui->comboBoxCPU->currentData().toInt()]));
    has_changed |= (cpu                        != ui->comboBoxSpeed->currentData().toInt());
    has_changed |= (fpu_type                   != ui->comboBoxFPU->currentData().toInt());
    has_changed |= (cpu_override_interpreter   != (ui->checkBoxOverrideInterpreter->isChecked() ? 1 : 0));
#ifdef USE_DYNAREC
    has_changed |= (cpu_use_dynarec            != (ui->checkBoxDynamicRecompiler->isChecked() ? 1 : 0));
    has_changed |= (cpu_dyn_accurate_fpu_env   != (ui->checkBoxAccurateFPUEnvironment->isChecked() ? 1 : 0));
#endif
    has_changed |= (fpu_mode_current()         != ui->comboBoxFPUMode->currentData().toInt());
    has_changed |= (force_10ms                 != (ui->radioButtonLargerFrames->isChecked() ? 1 : 0));
    has_changed |= (cpu_count                  != (ui->comboBoxProcessors->isEnabled() ? ui->comboBoxProcessors->currentData().toInt() : 1));
    has_changed |= (cpu_smp_drift_us           != ui->comboBoxSmpDrift->currentData().toInt());

    int64_t temp_mem_size;
    if (machine_get_ram_granularity(machine) < 1024)
        temp_mem_size = ui->spinBoxRAM->value();
    else
        temp_mem_size = ui->spinBoxRAM->value() * 1024;

    const int gran = machine_get_ram_granularity(machine);
    const int min  = machine_get_min_ram(machine);
    if (temp_mem_size > min)
        temp_mem_size = min + ((temp_mem_size - min) / gran) * gran;
    if (temp_mem_size < machine_get_min_ram(machine))
        temp_mem_size = machine_get_min_ram(machine);
    else if (temp_mem_size > machine_get_max_ram(machine))
        temp_mem_size = machine_get_max_ram(machine);
    has_changed |= (mem_size                   != static_cast<uint32_t>(temp_mem_size));

    if (ui->comboBoxWaitStates->isEnabled())
        temp = ui->comboBoxWaitStates->currentData().toInt();
    else
        temp = 0;
    has_changed |= (cpu_waitstates             != temp);

    has_changed |= (pit_mode                   !=  ui->comboBoxPitMode->currentData().toInt());

    temp = 0;
    if (ui->radioButtonLocalTime->isChecked())
        temp = TIME_SYNC_ENABLED;
    if (ui->radioButtonUTC->isChecked())
        temp = TIME_SYNC_ENABLED | TIME_SYNC_UTC;
    has_changed |= (time_sync                  != temp);

    return has_changed ? (SETTINGS_CHANGED | SETTINGS_REQUIRE_HARD_RESET) : 0;
}

void
SettingsMachine::restore()
{
}

int
SettingsMachine::currentMachineId() const
{
    return ui->comboBoxMachine->currentData().toInt();
}

void
SettingsMachine::save(int soft)
{
    if (soft)
        return;

    machine                  = ui->comboBoxMachine->currentData().toInt();
    cpu_f                    = const_cast<cpu_family_t *>(&cpu_families[ui->comboBoxCPU->currentData().toInt()]);
    cpu                      = ui->comboBoxSpeed->currentData().toInt();
    fpu_type                 = ui->comboBoxFPU->currentData().toInt();
    cpu_override_interpreter = ui->checkBoxOverrideInterpreter->isChecked() ? 1 : 0;
#ifdef USE_DYNAREC
    cpu_use_dynarec          = ui->checkBoxDynamicRecompiler->isChecked() ? 1 : 0;
    cpu_dyn_accurate_fpu_env = ui->checkBoxAccurateFPUEnvironment->isChecked() ? 1 : 0;
#endif
    fpu_softfloat            = (ui->comboBoxFPUMode->currentData().toInt() != FPU_MODE_DEFAULT) ? 1 : 0;
    fpu_host_x87             = (ui->comboBoxFPUMode->currentData().toInt() == FPU_MODE_HOST_X87) ? 1 : 0;
    force_10ms               = ui->radioButtonLargerFrames->isChecked() ? 1 : 0;
    cpu_count                = ui->comboBoxProcessors->isEnabled() ? ui->comboBoxProcessors->currentData().toInt() : 1;
    cpu_smp_drift_us         = ui->comboBoxSmpDrift->currentData().toInt();

    int64_t temp_mem_size;
    if (machine_get_ram_granularity(machine) < 1024)
        temp_mem_size = ui->spinBoxRAM->value();
    else
        temp_mem_size = ui->spinBoxRAM->value() * 1024;

    const int gran = machine_get_ram_granularity(machine);
    const int min  = machine_get_min_ram(machine);
    if (temp_mem_size > min)
        temp_mem_size = min + ((temp_mem_size - min) / gran) * gran;
    if (temp_mem_size < machine_get_min_ram(machine))
        temp_mem_size = machine_get_min_ram(machine);
    else if (temp_mem_size > machine_get_max_ram(machine))
        temp_mem_size = machine_get_max_ram(machine);
    mem_size = static_cast<uint32_t>(temp_mem_size);

    if (ui->comboBoxWaitStates->isEnabled())
        cpu_waitstates = ui->comboBoxWaitStates->currentData().toInt();
    else
        cpu_waitstates = 0;

    pit_mode = ui->comboBoxPitMode->currentData().toInt();

    time_sync = 0;
    if (ui->radioButtonLocalTime->isChecked())
        time_sync = TIME_SYNC_ENABLED;
    if (ui->radioButtonUTC->isChecked())
        time_sync = TIME_SYNC_ENABLED | TIME_SYNC_UTC;
}

void
SettingsMachine::on_comboBoxMachineType_currentIndexChanged(int index)
{
    if (index >= 0) {
        auto *model      = ui->comboBoxMachine->model();
        int   removeRows = model->rowCount();

        int selectedMachineRow = 0;
        {
            /* Swapping the list over moves the selection through machines
               nobody chose; only the one it lands on is announced, so the
               other pages are not rebuilt for the others, nor drop devices
               those machines cannot take. */
            const QSignalBlocker blocker(ui->comboBoxMachine);

            Models::Batch rows(model);
            for (int i = 0; i < machine_count(); ++i) {
                if ((machine_get_type(i) == ui->comboBoxMachineType->currentData().toInt()) && machine_available(i)) {
                    int row = rows.add(machines[i].name, i);
                    if (i == machine)
                        selectedMachineRow = row - removeRows;
                }
            }
            rows.commit();
            model->removeRows(0, removeRows);

            ui->comboBoxMachine->setCurrentIndex(-1);
        }
        ui->comboBoxMachine->setCurrentIndex(selectedMachineRow);
    }
}

void
SettingsMachine::on_comboBoxMachine_currentIndexChanged(int index)
{
    // win_settings_machine_recalc_machine
    if (index >= 0) {
        int         machineId = ui->comboBoxMachine->currentData().toInt();
        const auto *device    = machine_get_device(machineId);
        ui->pushButtonConfigure->setEnabled((device != nullptr) && (device->config != nullptr));

        auto *modelCpu   = ui->comboBoxCPU->model();
        int   removeRows = modelCpu->rowCount();

        int i                    = 0;
        int eligibleRows         = 0;
        int selectedCpuFamilyRow = 0;
        Models::Batch cpuRows(modelCpu);
        while (cpu_families[i].package != 0) {
            if (cpu_family_is_eligible(&cpu_families[i], machineId)) {
                cpuRows.add(QString("%1 %2").arg(cpu_families[i].manufacturer, cpu_families[i].name), i);
                if (&cpu_families[i] == cpu_f)
                    selectedCpuFamilyRow = eligibleRows;
                ++eligibleRows;
            }
            ++i;
        }
        cpuRows.commit();
        modelCpu->removeRows(0, removeRows);
        ui->comboBoxCPU->setEnabled(eligibleRows > 1);
        ui->comboBoxCPU->setCurrentIndex(-1);
        ui->comboBoxCPU->setCurrentIndex(selectedCpuFamilyRow);

        int divisor;
        if (machine_get_ram_granularity(machineId) < 1024) {
            divisor = 1;
            ui->spinBoxRAM->setSuffix(QCoreApplication::translate("", "KB").prepend(' '));
        } else {
            divisor = 1024;
            ui->spinBoxRAM->setSuffix(QCoreApplication::translate("", "MB").prepend(' '));
        }
        ui->spinBoxRAM->setMinimum(machine_get_min_ram(machineId) / divisor);
        ui->spinBoxRAM->setMaximum(machine_get_max_ram(machineId) / divisor);
        ui->spinBoxRAM->setSingleStep(machine_get_ram_granularity(machineId) / divisor);
        ui->spinBoxRAM->setValue(mem_size / divisor);
        ui->spinBoxRAM->setEnabled(machine_get_min_ram(machineId) != machine_get_max_ram(machineId));

        {
            bool dual = machine_cpu_sockets(machineId) > 1;

            ui->labelProcessors->setVisible(dual);
            ui->comboBoxProcessors->setVisible(dual);
            ui->labelSmpDrift->setVisible(dual);
            ui->comboBoxSmpDrift->setVisible(dual);
            updateProcessorsPairable();
        }

        emit currentMachineChanged(machineId);
    }
}

/* The previewed board's "disable_apic" setting, as machine_get_config_int
   reads the running one's. */
static bool
machine_disable_apic_set(int machineId)
{
    const device_t *dev = machine_get_device(machineId);

    if ((dev == nullptr) || (dev->config == nullptr))
        return false;
    for (const device_config_t *cfg = dev->config; cfg->type != CONFIG_END; cfg++)
        if (!strcmp(cfg->name, "disable_apic"))
            return config_get_int((char *) dev->name, (char *) "disable_apic", cfg->default_int) != 0;
    return false;
}

/* More than one processor only on a board with more than one socket, and
   with a processor that runs as a pair: one with an on-chip local APIC (the
   others start from a STARTUP), and not an OverDrive, the upgrade that
   takes the pair's place (82374EB 13.5.2). Anything else runs one, so One
   is what is shown and saved. */
void
SettingsMachine::updateProcessorsPairable()
{
    static const char *const names[] = { QT_TR_NOOP("One"), QT_TR_NOOP("Two"), QT_TR_NOOP("Three"), QT_TR_NOOP("Four") };
    const int   machineId   = ui->comboBoxMachine->currentData().toInt();
    const int   sockets     = machine_cpu_sockets(machineId);
    const bool  dual        = sockets > 1;
    bool        pairable    = false;
    const int   cpuFamilyId = ui->comboBoxCPU->currentData().toInt();
    const int   cpuId       = ui->comboBoxSpeed->currentData().toInt();

    if (dual && (ui->comboBoxCPU->currentIndex() >= 0) && (ui->comboBoxSpeed->currentIndex() >= 0))
        pairable = cpu_mp_pairable(&cpu_families[cpuFamilyId], &cpu_families[cpuFamilyId].cpus[cpuId]);

    /* The board's own "disable APIC" leaves no way to start a second. */
    const bool apic_off = dual && machine_disable_apic_set(machineId);

    ui->comboBoxProcessors->setEnabled(pairable && !apic_off);
    ui->comboBoxProcessors->setToolTip(!dual ? QString() :
                                       !pairable ? tr("This processor cannot run as a pair") :
                                       apic_off  ? tr("The board's APIC is disabled") : QString());
    /* One entry for each socket. Shown as One while it cannot be more; the
       choice comes back with a processor and board that can. */
    {
        const int wanted = wantedProcessors;

        ui->comboBoxProcessors->blockSignals(true);
        ui->comboBoxProcessors->model()->removeRows(0, ui->comboBoxProcessors->model()->rowCount());
        for (int n = 1; (n <= sockets) && (n <= (int) (sizeof(names) / sizeof(names[0]))); n++)
            Models::AddEntry(ui->comboBoxProcessors->model(), tr(names[n - 1]), n);
        ui->comboBoxProcessors->blockSignals(false);
        wantedProcessors = wanted;
    }
    ui->comboBoxProcessors->setCurrentIndex((pairable && !apic_off && (wantedProcessors > 1)) ?
                                            (((wantedProcessors < sockets) ? wantedProcessors : sockets) - 1) : 0);
    on_comboBoxProcessors_currentIndexChanged(ui->comboBoxProcessors->currentIndex());
}

/* The drift only means something with more than one processor. */
void
SettingsMachine::on_comboBoxProcessors_currentIndexChanged(int index)
{
    if (ui->comboBoxProcessors->isEnabled() && (index >= 0))
        wantedProcessors = ui->comboBoxProcessors->currentData().toInt();
    ui->comboBoxSmpDrift->setEnabled(ui->comboBoxProcessors->isEnabled() && (index >= 0) &&
                                     (ui->comboBoxProcessors->currentData().toInt() > 1));
    ui->labelSmpDrift->setEnabled(ui->comboBoxSmpDrift->isEnabled());
}

void
SettingsMachine::on_comboBoxCPU_currentIndexChanged(int index)
{
    if (index >= 0) {
        int         machineId   = ui->comboBoxMachine->currentData().toInt();
        int         cpuFamilyId = ui->comboBoxCPU->currentData().toInt();
        const auto *cpuFamily   = &cpu_families[cpuFamilyId];

        auto *modelSpeed = ui->comboBoxSpeed->model();
        int   removeRows = modelSpeed->rowCount();

        // win_settings_machine_recalc_cpu_m
        int i                = 0;
        int eligibleRows     = 0;
        int selectedSpeedRow = 0;
        Models::Batch speedRows(modelSpeed);
        while (cpuFamily->cpus[i].cpu_type != 0) {
            if (cpu_is_eligible(cpuFamily, i, machineId)) {
                speedRows.add(QString("%1").arg(cpuFamily->cpus[i].name), i);
                if (cpu == i)
                    selectedSpeedRow = eligibleRows;
                ++eligibleRows;
            }
            ++i;
        }
        speedRows.commit();
        modelSpeed->removeRows(0, removeRows);
        ui->comboBoxSpeed->setEnabled(eligibleRows > 1);
        ui->comboBoxSpeed->setCurrentIndex(-1);
        ui->comboBoxSpeed->setCurrentIndex(selectedSpeedRow);
    }
}

void
SettingsMachine::on_comboBoxSpeed_currentIndexChanged(int index)
{
    if (index >= 0) {
        // win_settings_machine_recalc_cpu
        int         cpuFamilyId = ui->comboBoxCPU->currentData().toInt();
        const auto *cpuFamily   = &cpu_families[cpuFamilyId];
        int         cpuId       = ui->comboBoxSpeed->currentData().toInt();
        uint        cpuType     = cpuFamily->cpus[cpuId].cpu_type;
        int         machineId   = ui->comboBoxMachine->currentData().toInt();
        /* The Tandy VIS has no wait states. */
        if ((cpuType >= CPU_286) && (cpuType < CPU_386DX) && machines[machineId].init != machine_at_tandy_vis_init) {
            ui->comboBoxWaitStates->setEnabled(true);
            ui->comboBoxWaitStates->setCurrentIndex(cpu_waitstates);
        } else {
            ui->comboBoxWaitStates->setCurrentIndex(0);
            ui->comboBoxWaitStates->setEnabled(false);
        }

#ifdef USE_DYNAREC
        uint8_t flags = cpuFamily->cpus[cpuId].cpu_flags;
        if (!(flags & CPU_SUPPORTS_DYNAREC)) {
            ui->checkBoxDynamicRecompiler->setChecked(false);
            ui->checkBoxDynamicRecompiler->setEnabled(false);
            ui->checkBoxAccurateFPUEnvironment->setEnabled(false);
        } else if ((flags & CPU_REQUIRES_DYNAREC) && !cpu_override) {
            ui->checkBoxDynamicRecompiler->setChecked(true);
            ui->checkBoxDynamicRecompiler->setEnabled(false);
            ui->checkBoxAccurateFPUEnvironment->setEnabled(true);
        } else {
            ui->checkBoxDynamicRecompiler->setChecked(cpu_use_dynarec);
            ui->checkBoxDynamicRecompiler->setEnabled(true);
            ui->checkBoxAccurateFPUEnvironment->setEnabled(cpu_use_dynarec);
        }
#endif

        ui->checkBoxAccurateFPUEnvironment->setChecked(cpu_dyn_accurate_fpu_env);

        if ((cpuType < CPU_286) || (cpuType == CPU_IBM486SLC) || (cpuType == CPU_IBM486BL) ||
            !strcmp(cpuFamily->manufacturer, "Cyrix") || !strcmp(cpuFamily->manufacturer, "ST") ||
            (cpuType > CPU_486DLC))
            ui->checkBoxOverrideInterpreter->setEnabled(false);
        else
            ui->checkBoxOverrideInterpreter->setEnabled(true);

        // win_settings_machine_recalc_fpu
        auto *modelFpu   = ui->comboBoxFPU->model();
        int   removeRows = modelFpu->rowCount();

        int i              = 0;
        int selectedFpuRow = 0;
        Models::Batch fpuRows(modelFpu);
        for (const char *fpuName         = fpu_get_name_from_index(cpuFamily, cpuId, i);
             fpuName != nullptr; fpuName = fpu_get_name_from_index(cpuFamily, cpuId, ++i)) {
            auto fpuType = fpu_get_type_from_index(cpuFamily, cpuId, i);
            fpuRows.add(tr(fpuName), fpuType);
            if (fpu_type == fpuType)
                selectedFpuRow = i;
        }

        fpuRows.commit();
        modelFpu->removeRows(0, removeRows);
        ui->comboBoxFPU->setEnabled(modelFpu->rowCount() > 1);
        ui->comboBoxFPU->setCurrentIndex(-1);
        ui->comboBoxFPU->setCurrentIndex(selectedFpuRow);

        updateProcessorsPairable();
    }
}

void
SettingsMachine::on_comboBoxFPU_currentIndexChanged(int index)
{
    if (index >= 0) {
        int         cpuFamilyId = ui->comboBoxCPU->currentData().toInt();
        const auto *cpuFamily   = &cpu_families[cpuFamilyId];
        int         cpuId       = ui->comboBoxSpeed->currentData().toInt();
        int         machineId   = ui->comboBoxMachine->currentData().toInt();

        auto *modes = qobject_cast<QStandardItemModel *>(ui->comboBoxFPUMode->model());

        if (fpu_get_type_from_index(cpuFamily, cpuId, index) == FPU_NONE) {
            ui->comboBoxFPUMode->setCurrentIndex(ui->comboBoxFPUMode->findData(FPU_MODE_DEFAULT));
            ui->comboBoxFPUMode->setEnabled(false);
        } else {
            /* A machine that only works with softfloat's FPU may take either
               exact one, but not the default. */
            const bool exact_only = machine_has_flags(machineId, MACHINE_SOFTFLOAT_ONLY);
            int        mode       = fpu_mode_current();

            if (exact_only && (mode == FPU_MODE_DEFAULT))
                mode = FPU_MODE_SOFTFLOAT;
            if (modes != nullptr)
                modes->item(ui->comboBoxFPUMode->findData(FPU_MODE_DEFAULT))->setEnabled(!exact_only);
            if (ui->comboBoxFPUMode->findData(mode) < 0)
                mode = exact_only ? FPU_MODE_SOFTFLOAT : FPU_MODE_DEFAULT;
            ui->comboBoxFPUMode->setCurrentIndex(ui->comboBoxFPUMode->findData(mode));
            ui->comboBoxFPUMode->setEnabled(true);
        }
    }
}

void
SettingsMachine::on_pushButtonConfigure_clicked()
{
    // deviceconfig_inst_open
    int         machineId = ui->comboBoxMachine->currentData().toInt();
    const auto *device    = machine_get_device(machineId);
    machine_cfg_changed |= DeviceConfig::ConfigureDevice(device);
    updateProcessorsPairable(); /* its "disable APIC" may have changed */
}

void
SettingsMachine::on_comboBoxFPUMode_currentIndexChanged(int index)
{
    if ((index >= 0) && (ui->comboBoxFPUMode->itemData(index).toInt() == FPU_MODE_SOFTFLOAT)) {
        ui->softFloatWarningIcon->setVisible(true);
        ui->softFloatWarningText->setVisible(true);
    } else {
        ui->softFloatWarningIcon->setVisible(false);
        ui->softFloatWarningText->setVisible(false);
    }
}

void
SettingsMachine::on_checkBoxDynamicRecompiler_stateChanged(int state)
{
#ifdef USE_DYNAREC
    ui->checkBoxAccurateFPUEnvironment->setEnabled(state);
#endif
}

void
SettingsMachine::on_radioButtonSmallerFrames_clicked()
{
    ui->radioButtonLargerFrames->setChecked(false);
}

void
SettingsMachine::on_radioButtonLargerFrames_clicked()
{
    ui->radioButtonSmallerFrames->setChecked(false);
}
