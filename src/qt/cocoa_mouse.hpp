#include <QAbstractNativeEventFilter>
#include <QByteArray>

#if QT_VERSION_MAJOR >= 6
#    define result_t qintptr
#else
#    define result_t long
#endif

class CocoaEventFilter : public QAbstractNativeEventFilter {
public:
    CocoaEventFilter() {};
    ~CocoaEventFilter();
    virtual bool nativeEventFilter(const QByteArray &eventType, void *message, result_t *result) override;

private:
    bool control_click_active = false;
    bool right_button_active  = false;
};

/* Hold the cursor where it is while the mouse is captured (macos_event_filter.mm). */
void macos_mouse_hold(int on);
