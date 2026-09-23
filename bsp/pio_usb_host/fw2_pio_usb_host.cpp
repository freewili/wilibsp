#include "fw2_pio_usb_host.h"
#include "fwUSBHost.h"
#include "fwUSBHostHIDController.h"
#include "tusb.h"

static fw2_usb_controller_cb s_controller_cb;
static fw2_usb_xinput_cb s_xinput_cb;

static void controller_thunk(uint8_t dev, uint8_t inst, const uint8_t *report, uint16_t len) {
    if (!s_controller_cb) return;
    uint16_t vid=0,pid=0; tuh_vid_pid_get(dev,&vid,&pid);
    s_controller_cb(dev,inst,report,len,vid,pid,
                    obUSBHost.m_obHID.getController().getPlayerForDevice(dev,inst));
}
static void xinput_thunk(uint8_t dev, uint8_t inst, const xinput_gamepad_t *p) {
    if (!s_xinput_cb) return;
    fw2_usb_xinput_pad_t out = { p->wButtons, p->sThumbLX, p->sThumbLY,
        p->sThumbRX, p->sThumbRY, p->bLeftTrigger, p->bRightTrigger };
    s_xinput_cb(&out, obUSBHost.m_obXInput.getPlayerForDevice(dev,inst));
}
bool fw2_pio_usb_host_init(void){return obUSBHost.init();}
void fw2_pio_usb_host_task(void){obUSBHost.task();}
void fw2_pio_usb_host_set_key_callback(fw2_usb_key_cb cb){obUSBHost.m_obHID.getKeyboard().setKeyCallback(cb);}
void fw2_pio_usb_host_set_controller_callback(fw2_usb_controller_cb cb){s_controller_cb=cb;obUSBHost.m_obHID.getController().setReportCallback(controller_thunk);}
void fw2_pio_usb_host_set_xinput_callback(fw2_usb_xinput_cb cb){s_xinput_cb=cb;obUSBHost.m_obXInput.setReportCallback(xinput_thunk);}
uint8_t fw2_pio_usb_host_modifiers(void){return obUSBHost.m_obHID.getKeyboard().getModifiers();}
bool fw2_pio_usb_host_mouse(int32_t *dx,int32_t *dy,int32_t *wheel,uint8_t *buttons){
    auto &m=obUSBHost.m_obHID.getMouse(); if(!m.isAnyMounted())return false;
    m.getDeltas(dx,dy,wheel);if(buttons)*buttons=m.getButtons();return true;
}
unsigned fw2_pio_usb_host_keyboard_count(void){return obUSBHost.m_obHID.getKeyboardCount();}
unsigned fw2_pio_usb_host_mouse_count(void){return obUSBHost.m_obHID.getMouseCount();}
unsigned fw2_pio_usb_host_controller_count(void){return obUSBHost.m_obHID.getControllerCount();}
unsigned fw2_pio_usb_host_xinput_count(void){return obUSBHost.m_obXInput.getMountedCount();}
