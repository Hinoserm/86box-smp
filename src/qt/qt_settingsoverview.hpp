#ifndef QT_SETTINGSOVERVIEW_HPP
#define QT_SETTINGSOVERVIEW_HPP

#include <QWidget>
#include <QSet>
#include <QHash>

#include <functional>

#include "qt_settingscards.hpp"

class QComboBox;
class QTimer;
class QTreeWidget;
class QTreeWidgetItem;
class SettingsHarddisks;
class SettingsFloppyCDROM;

/* Advanced: the machine as one tree, every device in it. A row says what
   the device is, with a drop-down where there is a choice of what it is,
   and Configure opens its settings in a window. It keeps nothing of its
   own: the drop-downs are the classic pages' controls. */
class SettingsOverview : public QWidget {
    Q_OBJECT

public:
    struct Pages {
        QWidget             *machine;
        QWidget             *input;
        QWidget             *ports;
        SettingsHarddisks   *harddisks;
        SettingsFloppyCDROM *floppyCdrom;
    };

    SettingsOverview(const Pages &pages, QWidget *parent = nullptr);

    void refreshSoon();

protected:
    void showEvent(QShowEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    Pages        pages;
    QTreeWidget *tree;
    QTimer      *timer;
    bool dirty = true;
    bool rebuilding = false;
    int renderedMachine = -1;
    QSet<QTreeWidgetItem *> visitedRows;
    QSet<QTreeWidgetItem *> newRows;
    int changedEditors = 0;
    bool materializingEditor = false;
    bool slotEditorReady = false;
    QHash<QTreeWidgetItem *, std::function<void()>> deferredEditors;
    void materializeEditor(QTreeWidgetItem *item);

    void refresh();

    QTreeWidgetItem *row(QTreeWidgetItem *parent, const QString &label, const QString &text = QString());
    QComboBox       *mirror(QTreeWidgetItem *item, QComboBox *source);
    void             putType(QTreeWidgetItem *item, QWidget *widget);
    void             configureButton(QTreeWidgetItem *item, const std::function<void()> &action, bool enabled = true);
    void             inWindow(const QString &title, const QList<QWidget *> &widgets);
    void             configureCard(const Cards::Card &card);

    void slotCombo(QTreeWidgetItem *item, const QString &bus, const QList<Cards::Card> &cards, int card, int slot);
    void processorCombo(QTreeWidgetItem *item);
    void memoryCombo(QTreeWidgetItem *item);
    void drivesUnder(QTreeWidgetItem *parent, bool scsi, int bus);
};

/* The renames and deletions chosen in the image manager, made only when
   the Settings are saved (apply()), and dropped on Cancel (discard()). */
namespace DriveImages {
void beginSession();
bool hasChanges();
void apply(QWidget *parent);
void discard();
}

/* The disk images in the machine's folder, used or not: new ones made
   and existing ones added through the drive pages, unused ones renamed or
   deleted when the Settings are saved. */
class SettingsDriveImages : public QWidget {
    Q_OBJECT

public:
    SettingsDriveImages(SettingsHarddisks *harddisks, QWidget *parent = nullptr);

    void refresh();

protected:
    void showEvent(QShowEvent *event) override;

private:
    SettingsHarddisks *harddisks;
    QTreeWidget       *list;
    QWidget           *buttonRename;
    QWidget           *buttonDelete;

    QString selected() const;
    QString selectedShown() const;
    void    selectionChanged();
};

#endif // QT_SETTINGSOVERVIEW_HPP
