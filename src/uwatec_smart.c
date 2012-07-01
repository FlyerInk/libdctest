/* 
 * libdivecomputer
 * 
 * Copyright (C) 2008 Jef Driesen
 * 
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 * 
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 * 
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston,
 * MA 02110-1301 USA
 */

#include <stdlib.h> // malloc, free
#include <string.h>	// strncmp, strstr

#include <libdivecomputer/uwatec_smart.h>

#include "context-private.h"
#include "device-private.h"
#include "irda.h"
#include "array.h"

#define EXITCODE(rc) \
( \
	rc == -1 ? DC_STATUS_IO : DC_STATUS_TIMEOUT \
)

typedef struct uwatec_smart_device_t {
	dc_device_t base;
	irda_t *socket;
	unsigned int address;
	unsigned int timestamp;
	unsigned int devtime;
	dc_ticks_t systime;
} uwatec_smart_device_t;

static dc_status_t uwatec_smart_device_set_fingerprint (dc_device_t *device, const unsigned char data[], unsigned int size);
static dc_status_t uwatec_smart_device_version (dc_device_t *abstract, unsigned char data[], unsigned int size);
static dc_status_t uwatec_smart_device_dump (dc_device_t *abstract, dc_buffer_t *buffer);
static dc_status_t uwatec_smart_device_foreach (dc_device_t *abstract, dc_dive_callback_t callback, void *userdata);
static dc_status_t uwatec_smart_device_close (dc_device_t *abstract);

static const device_backend_t uwatec_smart_device_backend = {
	DC_FAMILY_UWATEC_SMART,
	uwatec_smart_device_set_fingerprint, /* set_fingerprint */
	uwatec_smart_device_version, /* version */
	NULL, /* read */
	NULL, /* write */
	uwatec_smart_device_dump, /* dump */
	uwatec_smart_device_foreach, /* foreach */
	uwatec_smart_device_close /* close */
};

static int
device_is_uwatec_smart (dc_device_t *abstract)
{
	if (abstract == NULL)
		return 0;

    return abstract->backend == &uwatec_smart_device_backend;
}


static void
uwatec_smart_discovery (unsigned int address, const char *name, unsigned int charset, unsigned int hints, void *userdata)
{
	uwatec_smart_device_t *device = (uwatec_smart_device_t*) userdata;
	if (device == NULL)
		return;

	if (strncmp (name, "UWATEC Galileo Sol", 18) == 0 ||
		strncmp (name, "Uwatec Smart", 12) == 0 ||
		strstr (name, "Uwatec") != NULL ||
		strstr (name, "UWATEC") != NULL ||
		strstr (name, "Aladin") != NULL ||
		strstr (name, "ALADIN") != NULL ||
		strstr (name, "Smart") != NULL ||
		strstr (name, "SMART") != NULL ||
		strstr (name, "Galileo") != NULL ||
		strstr (name, "GALILEO") != NULL)
	{
		device->address = address;
	}
}


static dc_status_t
uwatec_smart_transfer (uwatec_smart_device_t *device, const unsigned char command[], unsigned int csize, unsigned char answer[], unsigned int asize)
{
	dc_device_t *abstract = (dc_device_t *) device;

	int n = irda_socket_write (device->socket, command, csize);
	if (n != csize) {
		ERROR (abstract->context, "Failed to send the command.");
		return EXITCODE (n);
	}

	n = irda_socket_read (device->socket, answer, asize);
	if (n != asize) {
		ERROR (abstract->context, "Failed to receive the answer.");
		return EXITCODE (n);
	}

	return DC_STATUS_SUCCESS;
}


static dc_status_t
uwatec_smart_handshake (uwatec_smart_device_t *device)
{
	dc_device_t *abstract = (dc_device_t *) device;

	// Command template.
	unsigned char answer[1] = {0};
	unsigned char command[5] = {0x00, 0x10, 0x27, 0, 0};

	// Handshake (stage 1).
	command[0] = 0x1B;
	dc_status_t rc = uwatec_smart_transfer (device, command, 1, answer, 1);
	if (rc != DC_STATUS_SUCCESS)
		return rc;

	// Verify the answer.
	if (answer[0] != 0x01) {
		ERROR (abstract->context, "Unexpected answer byte(s).");
		return DC_STATUS_PROTOCOL;
	}

	// Handshake (stage 2).
	command[0] = 0x1C;
	rc = uwatec_smart_transfer (device, command, 5, answer, 1);
	if (rc != DC_STATUS_SUCCESS)
		return rc;

	// Verify the answer.
	if (answer[0] != 0x01) {
		ERROR (abstract->context, "Unexpected answer byte(s).");
		return DC_STATUS_PROTOCOL;
	}

	return DC_STATUS_SUCCESS;
}


dc_status_t
uwatec_smart_device_open (dc_device_t **out, dc_context_t *context)
{
	if (out == NULL)
		return DC_STATUS_INVALIDARGS;

	// Allocate memory.
	uwatec_smart_device_t *device = (uwatec_smart_device_t *) malloc (sizeof (uwatec_smart_device_t));
	if (device == NULL) {
		ERROR (context, "Failed to allocate memory.");
		return DC_STATUS_NOMEMORY;
	}

	// Initialize the base class.
	device_init (&device->base, &uwatec_smart_device_backend);

	// Set the default values.
	device->socket = NULL;
	device->address = 0;
	device->timestamp = 0;
	device->systime = (dc_ticks_t) -1;
	device->devtime = 0;

	// Open the irda socket.
	int rc = irda_socket_open (&device->socket);
	if (rc == -1) {
		ERROR (context, "Failed to open the irda socket.");
		free (device);
		return DC_STATUS_IO;
	}

	// Discover the device.
	rc = irda_socket_discover (device->socket, uwatec_smart_discovery, device);
	if (rc == -1) {
		ERROR (context, "Failed to discover the device.");
		irda_socket_close (device->socket);
		free (device);
		return DC_STATUS_IO;
	}

	if (device->address == 0) {
		ERROR (context, "No dive computer found.");
		irda_socket_close (device->socket);
		free (device);
		return DC_STATUS_IO;
	}

	// Connect the device.
	rc = irda_socket_connect_lsap (device->socket, device->address, 1);
	if (rc == -1) {
		ERROR (context, "Failed to connect the device.");
		irda_socket_close (device->socket);
		free (device);
		return DC_STATUS_IO;
	}

	// Perform the handshaking.
	uwatec_smart_handshake (device);

	*out = (dc_device_t*) device;

	return DC_STATUS_SUCCESS;
}


static dc_status_t
uwatec_smart_device_close (dc_device_t *abstract)
{
	uwatec_smart_device_t *device = (uwatec_smart_device_t*) abstract;

	if (! device_is_uwatec_smart (abstract))
		return DC_STATUS_INVALIDARGS;

	// Close the device.
	if (irda_socket_close (device->socket) == -1) {
		free (device);
		return DC_STATUS_IO;
	}

	// Free memory.	
	free (device);

	return DC_STATUS_SUCCESS;
}


dc_status_t
uwatec_smart_device_set_timestamp (dc_device_t *abstract, unsigned int timestamp)
{
	uwatec_smart_device_t *device = (uwatec_smart_device_t*) abstract;

	if (! device_is_uwatec_smart (abstract))
		return DC_STATUS_INVALIDARGS;

	device->timestamp = timestamp;

	return DC_STATUS_SUCCESS;
}


static dc_status_t
uwatec_smart_device_set_fingerprint (dc_device_t *abstract, const unsigned char data[], unsigned int size)
{
	uwatec_smart_device_t *device = (uwatec_smart_device_t*) abstract;

	if (! device_is_uwatec_smart (abstract))
		return DC_STATUS_INVALIDARGS;

	if (size && size != 4)
		return DC_STATUS_INVALIDARGS;

	if (size)
		device->timestamp = array_uint32_le (data);
	else
		device->timestamp = 0;

	return DC_STATUS_SUCCESS;
}


static dc_status_t
uwatec_smart_device_version (dc_device_t *abstract, unsigned char data[], unsigned int size)
{
	uwatec_smart_device_t *device = (uwatec_smart_device_t *) abstract;

	if (size < UWATEC_SMART_VERSION_SIZE) {
		ERROR (abstract->context, "Insufficient buffer space available.");
		return DC_STATUS_NOMEMORY;
	}

	unsigned char command[1] = {0};

	// Model Number.
	command[0] = 0x10;
	dc_status_t rc = uwatec_smart_transfer (device, command, 1, data + 0, 1);
	if (rc != DC_STATUS_SUCCESS)
		return rc;

	// Serial Number.
	command[0] = 0x14;
	rc = uwatec_smart_transfer (device, command, 1, data + 1, 4);
	if (rc != DC_STATUS_SUCCESS)
		return rc;

	// Current Timestamp.
	command[0] = 0x1A;
	rc = uwatec_smart_transfer (device, command, 1, data + 5, 4);
	if (rc != DC_STATUS_SUCCESS)
		return rc;

	return DC_STATUS_SUCCESS;
}


static dc_status_t
uwatec_smart_device_dump (dc_device_t *abstract, dc_buffer_t *buffer)
{
	uwatec_smart_device_t *device = (uwatec_smart_device_t*) abstract;

	if (! device_is_uwatec_smart (abstract))
		return DC_STATUS_INVALIDARGS;

	// Erase the current contents of the buffer.
	if (!dc_buffer_clear (buffer)) {
		ERROR (abstract->context, "Insufficient buffer space available.");
		return DC_STATUS_NOMEMORY;
	}

	// Enable progress notifications.
	dc_event_progress_t progress = EVENT_PROGRESS_INITIALIZER;
	device_event_emit (&device->base, DC_EVENT_PROGRESS, &progress);

	// Read the version and clock data.
	unsigned char version[UWATEC_SMART_VERSION_SIZE] = {0};
	dc_status_t rc = uwatec_smart_device_version (abstract, version, sizeof (version));
	if (rc != DC_STATUS_SUCCESS)
		return rc;

	// Store the clock calibration values.
	device->systime = dc_datetime_now ();
	device->devtime = array_uint32_le (version + 5);

	// Update and emit a progress event.
	progress.current += 9;
	device_event_emit (&device->base, DC_EVENT_PROGRESS, &progress);

	// Emit a clock event.
	dc_event_clock_t clock;
	clock.systime = device->systime;
	clock.devtime = device->devtime;
	device_event_emit (&device->base, DC_EVENT_CLOCK, &clock);

	// Emit a device info event.
	dc_event_devinfo_t devinfo;
	devinfo.model = version[0];
	devinfo.firmware = 0;
	devinfo.serial = array_uint32_le (version + 1);
	device_event_emit (&device->base, DC_EVENT_DEVINFO, &devinfo);

	// Command template.
	unsigned char answer[4] = {0};
	unsigned char command[9] = {0x00,
			(device->timestamp      ) & 0xFF,
			(device->timestamp >> 8 ) & 0xFF,
			(device->timestamp >> 16) & 0xFF,
			(device->timestamp >> 24) & 0xFF,
			0x10,
			0x27,
			0,
			0};

	// Data Length.
	command[0] = 0xC6;
	rc = uwatec_smart_transfer (device, command, sizeof (command), answer, sizeof (answer));
	if (rc != DC_STATUS_SUCCESS)
		return rc;

	unsigned int length = array_uint32_le (answer);

	// Update and emit a progress event.
	progress.maximum = 4 + 9 + (length ? length + 4 : 0);
	progress.current += 4;
	device_event_emit (&device->base, DC_EVENT_PROGRESS, &progress);

  	if (length == 0)
		return DC_STATUS_SUCCESS;

	// Allocate the required amount of memory.
	if (!dc_buffer_resize (buffer, length)) {
		ERROR (abstract->context, "Insufficient buffer space available.");
		return DC_STATUS_NOMEMORY;
	}

	unsigned char *data = dc_buffer_get_data (buffer);

	// Data.
	command[0] = 0xC4;
	rc = uwatec_smart_transfer (device, command, sizeof (command), answer, sizeof (answer));
	if (rc != DC_STATUS_SUCCESS)
		return rc;

	unsigned int total = array_uint32_le (answer);

	// Update and emit a progress event.
	progress.current += 4;
	device_event_emit (&device->base, DC_EVENT_PROGRESS, &progress);

	if (total != length + 4) {
		ERROR (abstract->context, "Received an unexpected size.");
		return DC_STATUS_PROTOCOL;
	}

	unsigned int nbytes = 0;
	while (nbytes < length) {
		// Set the minimum packet size.
		unsigned int len = 32;

		// Increase the packet size if more data is immediately available.
		int available = irda_socket_available (device->socket);
		if (available > len)
			len = available;

		// Limit the packet size to the total size.
		if (nbytes + len > length)
			len = length - nbytes;

		int n = irda_socket_read (device->socket, data + nbytes, len);
		if (n != len) {
			ERROR (abstract->context, "Failed to receive the answer.");
			return EXITCODE (n);
		}

		// Update and emit a progress event.
		progress.current += n;
		device_event_emit (&device->base, DC_EVENT_PROGRESS, &progress);

		nbytes += n;
	}

	return DC_STATUS_SUCCESS;
}


static dc_status_t
uwatec_smart_device_foreach (dc_device_t *abstract, dc_dive_callback_t callback, void *userdata)
{
	if (! device_is_uwatec_smart (abstract))
		return DC_STATUS_INVALIDARGS;

	dc_buffer_t *buffer = dc_buffer_new (0);
	if (buffer == NULL)
		return DC_STATUS_NOMEMORY;

	dc_status_t rc = uwatec_smart_device_dump (abstract, buffer);
	if (rc != DC_STATUS_SUCCESS) {
		dc_buffer_free (buffer);
		return rc;
	}

	rc = uwatec_smart_extract_dives (abstract,
		dc_buffer_get_data (buffer), dc_buffer_get_size (buffer), callback, userdata);

	dc_buffer_free (buffer);

	return rc;
}


dc_status_t
uwatec_smart_extract_dives (dc_device_t *abstract, const unsigned char data[], unsigned int size, dc_dive_callback_t callback, void *userdata)
{
	if (abstract && !device_is_uwatec_smart (abstract))
		return DC_STATUS_INVALIDARGS;

	const unsigned char header[4] = {0xa5, 0xa5, 0x5a, 0x5a};

	// Search the data stream for start markers.
	unsigned int previous = size;
	unsigned int current = (size >= 4 ? size - 4 : 0);
	while (current > 0) {
		current--;
		if (memcmp (data + current, header, sizeof (header)) == 0) {
			// Get the length of the profile data.
			unsigned int len = array_uint32_le (data + current + 4);

			// Check for a buffer overflow.
			if (current + len > previous)
				return DC_STATUS_DATAFORMAT;

			if (callback && !callback (data + current, len, data + current + 8, 4, userdata))
				return DC_STATUS_SUCCESS;

			// Prepare for the next dive.
			previous = current;
			current = (current >= 4 ? current - 4 : 0);
		}
	}

	return DC_STATUS_SUCCESS;
}
