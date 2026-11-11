/* Copyright (C) 2026 kichikuou <KichikuouChrome@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <http://gnu.org/licenses/>.
 */

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include "system4.h"
#include "system4/ain.h"
#include "system4/string.h"

#include "vm.h"
#include "parts.h"
#include "parts_internal.h"

#define PARTS_MSG_MAX_VARS 6

struct parts_message {
	STAILQ_ENTRY(parts_message) entry;
	int parts_no;
	int delegate_index;
	int type;
	struct {
		int type;
		union { int i; float f; bool b; struct string *s; } value;
	} variables[PARTS_MSG_MAX_VARS];
	int nr_variables;
};

static STAILQ_HEAD(, parts_message) msg_queue =
		STAILQ_HEAD_INITIALIZER(msg_queue);

static int message_type_v12(int type)
{
	// CPartsMessageManager.CallDelegate in the v12 scripts. MouseEnter is 0;
	// double-click and key-press occupy slots absent from the older interface.
	switch (type) {
	case PARTS_MSG_MOUSE_ENTER: return 0;
	case PARTS_MSG_MOUSE_MOVE: return 1;
	case PARTS_MSG_MOUSE_LEAVE: return 2;
	case PARTS_MSG_MOUSE_WHEEL: return 3;
	case PARTS_MSG_MOUSE_CLICK: return 4;
	case PARTS_MSG_MOUSE_ON: return 6;
	case PARTS_MSG_DRAG_BEGIN: return 7;
	case PARTS_MSG_DRAGGING: return 8;
	case PARTS_MSG_DRAG_END: return 10;
	case PARTS_MSG_DROP_ENTER: return 11;
	case PARTS_MSG_DROP_ON: return 12;
	case PARTS_MSG_DROPPED: return 13;
	case PARTS_MSG_DROP_LEAVE: return 14;
	case PARTS_MSG_KEY_TRIGGER: return 15;
	case PARTS_MSG_KEY_DOWN: return 16;
	case PARTS_MSG_KEY_UP: return 18;
	case PARTS_MSG_FOCUS_IN: return 19;
	case PARTS_MSG_FOCUS_OUT: return 20;
	case PARTS_MSG_CHECK_CHANGED: return 23;
	case PARTS_MSG_SCROLL: return 21;
	case PARTS_MSG_RADIO_BUTTON_BOX: return 22;
	case PARTS_MSG_SWIPE_BEGIN: return 27;
	case PARTS_MSG_SWIPING: return 28;
	case PARTS_MSG_SWIPE_END: return 29;
	default: VM_ERROR("Invalid internal Parts message type %d", type);
	}
}

void parts_msg_push(struct parts* parts, int type, const char *fmt, ...)
{
	// Message system is introduced in Rance 9
	if (!parts_multi_controller)
		return;
	if (ain->version >= 12 && parts && parts->delegate_index < 0)
		return;

	struct parts_message *msg = xmalloc(sizeof(*msg));
	msg->parts_no = parts ? parts->no : 0;
	msg->delegate_index = parts ? parts->delegate_index : -1;
	msg->type = ain->version >= 12 ? message_type_v12(type) : type;
	msg->nr_variables = 0;

	va_list ap;
	va_start(ap, fmt);
	for (const char *p = fmt; *p; p++) {
		if (msg->nr_variables >= PARTS_MSG_MAX_VARS) {
			VM_ERROR("parts_msg_push: too many variables");
		}
		switch (*p) {
		case 'i':
			msg->variables[msg->nr_variables].type = 1;
			msg->variables[msg->nr_variables++].value.i = va_arg(ap, int);
			break;
		case 'f':
			msg->variables[msg->nr_variables].type = 2;
			msg->variables[msg->nr_variables++].value.f = va_arg(ap, double);
			break;
		case 'b':
			msg->variables[msg->nr_variables].type = 3;
			msg->variables[msg->nr_variables++].value.b = va_arg(ap, int);
			break;
		case 's':
			msg->variables[msg->nr_variables].type = 4;
			msg->variables[msg->nr_variables++].value.s = string_dup(va_arg(ap, struct string*));
			break;
		default:
			VM_ERROR("parts_msg_push: unsupported format '%c'", *p);
		}
	}
	va_end(ap);

	STAILQ_INSERT_TAIL(&msg_queue, msg, entry);
}

static void message_free(struct parts_message *msg)
{
	for (int i = 0; i < msg->nr_variables; i++)
		if (msg->variables[i].type == 4) free_string(msg->variables[i].value.s);
	free(msg);
}

void PE_ReleaseMessage(void)
{
	while (!STAILQ_EMPTY(&msg_queue)) {
		struct parts_message *msg = STAILQ_FIRST(&msg_queue);
		STAILQ_REMOVE_HEAD(&msg_queue, entry);
		message_free(msg);
	}
}

void PE_PopMessage(void)
{
	if (STAILQ_EMPTY(&msg_queue))
		return;
	struct parts_message *msg = STAILQ_FIRST(&msg_queue);
	STAILQ_REMOVE_HEAD(&msg_queue, entry);
	message_free(msg);
}

int PE_GetMessageType(void)
{
	struct parts_message *msg = STAILQ_FIRST(&msg_queue);
	// v12 scripts drain the queue until -1. Zero is a valid message type.
	return msg ? msg->type : (ain->version >= 12 ? -1 : 0);
}

int PE_GetMessagePartsNumber(void)
{
	struct parts_message *msg = STAILQ_FIRST(&msg_queue);
	return msg ? msg->parts_no : 0;
}

int PE_GetMessageDelegateIndex(void)
{
	struct parts_message *msg = STAILQ_FIRST(&msg_queue);
	return msg ? msg->delegate_index : -1;
}

int PE_GetMessageVariableCount(void)
{
	struct parts_message *msg = STAILQ_FIRST(&msg_queue);
	return msg ? msg->nr_variables : 0;
}

int PE_GetMessageVariableType(int index)
{
	struct parts_message *msg = STAILQ_FIRST(&msg_queue);
	return msg && index >= 0 && index < msg->nr_variables ? msg->variables[index].type : 0;
}

int PE_GetMessageVariableInt(int index)
{
	struct parts_message *msg = STAILQ_FIRST(&msg_queue);
	if (!msg)
		return 0;
	if (index < 0 || index >= msg->nr_variables)
		return 0;
	return msg->variables[index].type == 1 ? msg->variables[index].value.i : 0;
}

float PE_GetMessageVariableFloat(int index)
{
	struct parts_message *msg = STAILQ_FIRST(&msg_queue);
	return PE_GetMessageVariableType(index) == 2 ? msg->variables[index].value.f : 0.0f;
}

bool PE_GetMessageVariableBool(int index)
{
	struct parts_message *msg = STAILQ_FIRST(&msg_queue);
	return PE_GetMessageVariableType(index) == 3 && msg->variables[index].value.b;
}

void PE_GetMessageVariableString(int index, struct string **out)
{
	if (*out)
		free_string(*out);
	struct parts_message *msg = STAILQ_FIRST(&msg_queue);
	*out = string_ref(PE_GetMessageVariableType(index) == 4 ? msg->variables[index].value.s : &EMPTY_STRING);
}
