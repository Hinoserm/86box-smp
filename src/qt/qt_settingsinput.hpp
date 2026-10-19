#ifndef QT_SETTINGSINPUT_HPP
#define QT_SETTINGSINPUT_HPP

#include <QWidget>
#include <QtGui/QStandardItemModel>
#include <QtGui/QStandardItem>
#include <QItemDelegate>
#include <QPainter>
#include <QVariant>
#include <QTableWidget>

class QComboBox;
class QPushButton;

namespace Ui {
class SettingsInput;
}

class SettingsInput : public QWidget {
    Q_OBJECT

public:
    explicit SettingsInput(QWidget *parent = nullptr);
    ~SettingsInput();

    int  changed();

    void restore();
    void save(int soft);

public slots:
    void onCurrentMachineChanged(int machineId);

private slots:
    void on_comboBoxKeyboard_currentIndexChanged(int index);
    void on_pushButtonConfigureKeyboard_clicked();

    void on_comboBoxMouse_currentIndexChanged(int index);
    void on_pushButtonConfigureMouse_clicked();

    void on_comboBoxTablet_currentIndexChanged(int index);
    void on_pushButtonConfigureTablet_clicked();

    void on_comboBoxJoystick0_currentIndexChanged(int index);
    void on_pushButtonJoystick01_clicked();
    void on_pushButtonJoystick02_clicked();
    void on_pushButtonJoystick03_clicked();
    void on_pushButtonJoystick04_clicked();

private:
    Ui::SettingsInput *ui;

    int                kbd_config_changed    = 0;
    int                mouse_config_changed  = 0;
    int                tablet_config_changed = 0;

    int                keyboardType = 0;
    int                mouseType = 0;
    int                tabletType = 0;
    int                joystickType = 0;
    int                joystick2Type = 0;
    int                gameport_card_config_changed = 0;

    /* The second game port's joystick and the game port card, made here
       beside the first joystick's controls. */
    QComboBox   *comboBoxJoystick1;
    QPushButton *joystick1Buttons[4];
    QComboBox   *comboBoxGameportCard;
    QPushButton *pushButtonConfigureGameportCard;

    void fillJoysticks(QComboBox *cbox, int current);
    void joystickChosen(int gameport);

    int                machineId = 0;

    SettingsCompleter   *scKeyboard;
    SettingsCompleter   *scMouse;
    SettingsCompleter   *scTablet;

    SettingsCompleter   *scJoystick0;
};

#endif // QT_SETTINGSINPUT_HPP
