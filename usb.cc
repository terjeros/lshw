/*
 * usb.cc
 *
 */

#include "usb.h"
#include "osutils.h"
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>

#define PROCBUSUSB "/proc/bus/usb/"

#define	USB_DT_DEVICE_SIZE	0x12
#define	USB_DT_CONFIG_SIZE	9
#define USB_CTRL_TIMEOUT    5000	/* milliseconds */
#define USB_CTRL_RETRIES     2

#define USB_CLASS_PER_INTERFACE         0	/* for DeviceClass */
#define USB_CLASS_AUDIO                 1
#define USB_CLASS_COMM                  2
#define USB_CLASS_HID                   3
#define USB_CLASS_PRINTER               7
#define USB_CLASS_MASS_STORAGE          8
#define USB_CLASS_HUB                   9
#define USB_CLASS_DATA                  10
#define USB_CLASS_VENDOR_SPEC           0xff

#define USB_DIR_OUT                     0
#define USB_DIR_IN                      0x80

/*
 * Descriptor types
 */
#define USB_DT_DEVICE                   0x01
#define USB_DT_CONFIG                   0x02
#define USB_DT_STRING                   0x03
#define USB_DT_INTERFACE                0x04
#define USB_DT_ENDPOINT                 0x05

#define USB_DT_HID                      (USB_TYPE_CLASS | 0x01)
#define USB_DT_REPORT                   (USB_TYPE_CLASS | 0x02)
#define USB_DT_PHYSICAL                 (USB_TYPE_CLASS | 0x03)
#define USB_DT_HUB                      (USB_TYPE_CLASS | 0x09)

/*
 * Standard requests
 */
#define USB_REQ_GET_STATUS              0x00
#define USB_REQ_CLEAR_FEATURE           0x01
#define USB_REQ_SET_FEATURE             0x03
#define USB_REQ_SET_ADDRESS             0x05
#define USB_REQ_GET_DESCRIPTOR          0x06
#define USB_REQ_SET_DESCRIPTOR          0x07
#define USB_REQ_GET_CONFIGURATION       0x08
#define USB_REQ_SET_CONFIGURATION       0x09
#define USB_REQ_GET_INTERFACE           0x0A
#define USB_REQ_SET_INTERFACE           0x0B
#define USB_REQ_SYNCH_FRAME             0x0C

/* All standard descriptors have these 2 fields in common */
struct usb_descriptor_header
{
  u_int8_t bLength;
  u_int8_t bDescriptorType;
}
__attribute__ ((packed));

/* Device descriptor */
struct usb_device_descriptor
{
  u_int8_t bLength;
  u_int8_t bDescriptorType;
  u_int16_t bcdUSB;
  u_int8_t bDeviceClass;
  u_int8_t bDeviceSubClass;
  u_int8_t bDeviceProtocol;
  u_int8_t bMaxPacketSize0;
  u_int16_t idVendor;
  u_int16_t idProduct;
  u_int16_t bcdDevice;
  u_int8_t iManufacturer;
  u_int8_t iProduct;
  u_int8_t iSerialNumber;
  u_int8_t bNumConfigurations;
}
__attribute__ ((packed));

struct usb_config_descriptor
{
  u_int8_t bLength;
  u_int8_t bDescriptorType;
  u_int16_t wTotalLength;
  u_int8_t bNumInterfaces;
  u_int8_t bConfigurationValue;
  u_int8_t iConfiguration;
  u_int8_t bmAttributes;
  u_int8_t MaxPower;

  struct usb_interface *interface;
}
__attribute__ ((packed));

#define USBDEVICE_SUPER_MAGIC 0x9fa2

/* usbdevfs ioctl codes */

struct usbdevfs_ctrltransfer
{
  u_int8_t requesttype;
  u_int8_t request;
  u_int16_t value;
  u_int16_t index;
  u_int16_t length;
  u_int32_t timeout;		/* in milliseconds */
  void *data;
};

#define USBDEVFS_CONTROL	_IOWR('U', 0, struct usbdevfs_ctrltransfer)

static char *id = "@(#) $Id: usb.cc,v 1.8 2004/02/26 16:52:33 ezix Exp $";

static int usb_control_msg(int fd,
			   u_int8_t requesttype,
			   u_int8_t request,
			   u_int16_t value,
			   u_int16_t index,
			   unsigned int size,
			   void *data)
{
  struct usbdevfs_ctrltransfer ctrl;
  int result, tries;

  ctrl.requesttype = requesttype;
  ctrl.request = request;
  ctrl.value = value;
  ctrl.index = index;
  ctrl.length = size;
  ctrl.data = data;
  ctrl.timeout = USB_CTRL_TIMEOUT;
  tries = 0;
  do
  {
    result = ioctl(fd, USBDEVFS_CONTROL, &ctrl);
    tries++;
  }
  while (tries < USB_CTRL_RETRIES && result == -1 && errno == ETIMEDOUT);
  return result;
}

static string get_string(int fd,
			 u_int8_t id,
			 u_int16_t lang)
{
  unsigned char b[256];
  wchar_t w[128];
  char buf[256];
  int i;
  int ret;

  /*
   * string ID 0 means no string 
   */
  if (!id || fd < 0)
    return "";

  memset(b, 0, sizeof(b));
  b[0] = b[1] = 0xbf;
  ret =
    usb_control_msg(fd, USB_DIR_IN, USB_REQ_GET_DESCRIPTOR,
		    (USB_DT_STRING << 8) | id, 0, sizeof(b), b);
  if (ret < 0)
  {
    return "";
  }
  if (ret < 2 || b[0] < 2 || b[1] != USB_DT_STRING)
  {
    return "";
  }

  for (i = 0; i < ((b[0] - 2) / 2); i++)
    w[i] = b[2 + 2 * i] | (b[3 + 2 * i] << 8);
  w[i] = 0;

  wcstombs(buf, w, sizeof(buf));

  return string(buf);
}

static bool get_config(int fd,
		       u_int8_t id,
		       usb_config_descriptor & config)
{
  memset(&config, 0, sizeof(config));

  if (usb_control_msg
      (fd, USB_DIR_IN, USB_REQ_GET_DESCRIPTOR, (USB_DT_CONFIG << 8) | id, 0,
       sizeof(config), &config) >= 0)
    return true;
  else
  {
    memset(&config, 0, sizeof(config));	// just to be sure
    return false;
  }
}

static string BCDversion(u_int16_t v)
{
  char version[20];

  snprintf(version, sizeof(version), "%x.%02x", (v & 0xff00) >> 8,
	   v & 0x00ff);

  return version;
}

static int selectnumber(const struct dirent *d)
{
  char *ptr = NULL;

  strtoul(d->d_name, &ptr, 10);
  return (ptr != NULL) && (strlen(ptr) == 0);
}

bool scan_usb(hwNode & n)
{
  hwNode *controller = NULL;
  struct dirent **hubs;
  int count;

  if (!exists(PROCBUSUSB))
    return false;

  if (!controller)
  {
    controller = n.addChild(hwNode("usb", hw::bus));

    if (controller)
      controller->setDescription("USB controller");
  }

  if (!controller)
    return false;

  controller->claim();

  pushd(PROCBUSUSB);
  count = scandir(".", &hubs, selectnumber, alphasort);

  for (int i = 0; i < count; i++)
  {
    hwNode hub("hub",
	       hw::bus);

    hub.claim();
    hub.setPhysId(hubs[i]->d_name);
    hub.setDescription("USB root hub");

    if (pushd(hubs[i]->d_name))
    {
      int countdevices;
      struct dirent **devices;
      countdevices = scandir(".", &devices, selectnumber, alphasort);

      for (int j = 0; j < countdevices; j++)
      {
	hwNode device("usb");
	usb_device_descriptor descriptor;
	int fd = -1;

	// device.setPhysId(devices[j]->d_name);
	device.setHandle(string("USB:") + string(hubs[i]->d_name) + ":" +
			 string(devices[j]->d_name));

	memset(&descriptor, sizeof(descriptor), 0);
	fd = open(devices[j]->d_name, O_RDWR);
	if (fd >= 0)
	{
	  if (read(fd, &descriptor, USB_DT_DEVICE_SIZE) == USB_DT_DEVICE_SIZE)
	  {
	    if (descriptor.bLength == USB_DT_DEVICE_SIZE)
	    {
	      device.addCapability("usb-" + BCDversion(descriptor.bcdUSB));
	      if (descriptor.bcdDevice)
		device.setVersion(BCDversion(descriptor.bcdDevice));

	      device.setProduct(get_string(fd, descriptor.iProduct, 0));
	      device.setVendor(get_string(fd, descriptor.iManufacturer, 0));
	      device.setSerial(get_string(fd, descriptor.iSerialNumber, 0));

	      switch (descriptor.bDeviceClass)
	      {
	      case USB_CLASS_AUDIO:
		device.setClass(hw::multimedia);
		device.setDescription("USB audio device");
		break;
	      case USB_CLASS_COMM:
		device.setClass(hw::communication);
		device.setDescription("USB communication device");
		break;
	      case USB_CLASS_HID:
		device.setClass(hw::input);
		device.setDescription("USB human interface device");
		break;
	      case USB_CLASS_PRINTER:
		device.setClass(hw::printer);
		device.setDescription("USB printer");
		break;
	      case USB_CLASS_MASS_STORAGE:
		device.setClass(hw::storage);
		device.setDescription("USB mass storage device");
		break;
	      case USB_CLASS_HUB:
		device.setClass(hw::bus);
		device.setDescription("USB hub");
		break;
	      }

	      if (descriptor.idVendor == 0)
		device.addCapability("virtual");

	      if (device.getClass() == hw::bus && device.isCapable("virtual"))
	      {
		device.setHandle(string("USB:") + string(hubs[i]->d_name));
		device.setBusInfo("usb@" + string(hubs[i]->d_name));
		device.setPhysId(string(hubs[i]->d_name));
	      }

	      for (unsigned int i = 0; i < descriptor.bNumConfigurations; i++)
	      {
		usb_config_descriptor config;

		if (get_config(fd, i, config))
		{
		  //printf("config: %d\n", config.iConfiguration);
		}
	      }
	    }
	  }
	  close(fd);
	}

	hub.addChild(device);
      }

      popd();
    }

    controller->addChild(hub);
  }
  popd();

  (void) &id;			// avoid warning "id defined but not used"

  return false;
}
