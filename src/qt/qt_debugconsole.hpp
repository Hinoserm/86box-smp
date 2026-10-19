#ifndef QT_DEBUGCONSOLE_HPP
#define QT_DEBUGCONSOLE_HPP

#include <QDialog>
#include <QHash>
#include <QList>
#include <QStringList>

class QPlainTextEdit;
class QLineEdit;
class QPushButton;
class QTcpServer;
class QTcpSocket;

/* Registers and memory of the paused machine, GDB style. Commands go to
   the emulation thread, which runs them from its paused loop. */
class DebugConsole : public QDialog {
    Q_OBJECT

public:
    explicit DebugConsole(QWidget *parent = nullptr);
    ~DebugConsole() override;

    void append(const QString &text);
    void deliver(int source, const QString &text);
    /* Share the console over TCP on 127.0.0.1:port now (debug_console_port). */
    void listenOn(int port);

protected:
    bool eventFilter(QObject *obj, QEvent *event) override;

private:
    void run(const QString &text);
    void enqueue(int source, const QString &line);
    void pump();
    bool control(int source, const QString &line);
    void toggleServer();
    void newClient();
    void clientData(QTcpSocket *sock);
    bool pasteLines(const QString &clip);
    void dumpMemory();
    void dumpRegisters();

    QPlainTextEdit *output;
    QLineEdit      *input;
    QStringList     history;
    int             historyPos = 0;
    QString         lastDumpPath;
    QString         lastRegsPath;

    /* Every console's lines, in the order they came, one at a time: a
       command goes to the emulation thread only when the one before it has
       answered, so pause, continue and hardreset act in their place. */
    struct Line {
        int     source;
        QString text;
    };
    QList<Line> pending;
    bool        inFlight = false;

    /* The same console, shared over TCP, on the loopback interface only. */
    QLineEdit               *portEdit;
    QPushButton             *serverButton;
    QTcpServer              *server = nullptr;
    QHash<int, QTcpSocket *> clients;
    int                      nextClient = 1;
};

#endif
