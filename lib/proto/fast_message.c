#include "libtrading/proto/fast_message.h"

#include "libtrading/read-write.h"
#include "libtrading/buffer.h"
#include "libtrading/array.h"

#include <string.h>
#include <stdlib.h>

static ssize_t data_read(struct buffer *buffer)
{
	ssize_t nr = 0;
	size_t size;
	int *fd;

	fd = buffer_get_ptr(buffer);

	size = buffer_remaining(buffer);
	if (size <= FAST_MESSAGE_MAX_SIZE)
		buffer_compact(buffer);

	/*
	 * If buffer's capacity is at least
	 * 2 times FAST_MESSAGE_MAX_SIZE then,
	 * remaining > FAST_MESSAGE_MAX_SIZE
	 */
	nr = buffer_nread(buffer, *fd, FAST_MESSAGE_MAX_SIZE);

	return nr;
}

static int parse_uint(struct buffer *buffer, u64 *value)
{
	const int bytes = 9;
	u64 result;
	int i;
	u8 c;

retry:
	result = 0;

	for (i = 0; i < bytes; i++) {
		if (!buffer_size(buffer))
			goto partial;

		c = buffer_first_char(buffer);
		buffer_advance(buffer, 1);

		if (c & 0x80) {
			result = (result << 7) | (c & 0x7F);
			*value = result;

			return 0;
		}

		result = (result << 7) | c;
	}

fail:
	return FAST_MSG_STATE_GARBLED;

partial:
	buffer_advance(buffer, -i);

	if (data_read(buffer) > 0)
		goto retry;
	else
		goto fail;
}

static int parse_int(struct buffer *buffer, i64 *value)
{
	const int bytes = 9;
	i64 result;
	int i;
	u8 c;

retry:
	result = 0;
	i = 0;

	if (!buffer_size(buffer))
		goto partial;

	if (buffer_first_char(buffer) & 0x40)
		result = -1;

	for (i = 0; i < bytes; i++) {
		if (!buffer_size(buffer))
			goto partial;

		c = buffer_first_char(buffer);
		buffer_advance(buffer, 1);

		if (c & 0x80) {
			result = (result << 7) | (c & 0x7F);
			*value = result;

			return 0;
		}

		result = (result << 7) | c;
	}

fail:
	return FAST_MSG_STATE_GARBLED;

partial:
	buffer_advance(buffer, -i);

	if (data_read(buffer) > 0)
		goto retry;
	else
		goto fail;
}

/*
 * If ASCII string is nullable, an additional zero-preamble
 * is allowed at the start of the string. Hence the function
 * returns the number of bytes read. Negative return value
 * indicates an error. If the return value is equal to 1 and
 * the string is nullable, it means that the string is NULL.
 */
static int parse_string(struct buffer *buffer, char *value)
{
	int len;
	u8 c;

retry:
	len = 0;

	while (len < FAST_STRING_MAX_BYTES - 1) {
		if (!buffer_size(buffer))
			goto partial;

		c = buffer_first_char(buffer);
		buffer_advance(buffer, 1);

		if (c & 0x80) {
			value[len++] = c & 0x7F;
			value[len] = '\0';

			return len;
		} else
			value[len++] = c;
	}

fail:
	return FAST_MSG_STATE_GARBLED;

partial:
	buffer_advance(buffer, -len);

	if (data_read(buffer) > 0)
		goto retry;
	else
		goto fail;
}

static int parse_bytes(struct buffer *buffer, char *value, int len)
{
	int i;
	u8 c;

retry:
	if (buffer_size(buffer) < len)
		goto partial;

	for (i = 0; i < len; i++) {
		c = buffer_first_char(buffer);
		buffer_advance(buffer, 1);

		value[i] = c;
	}

	return 0;

fail:
	return FAST_MSG_STATE_GARBLED;

partial:
	if (data_read(buffer) > 0)
		goto retry;
	else
		goto fail;
}

static int parse_pmap(struct buffer *buffer, struct fast_pmap *pmap)
{
	char c;

retry:
	pmap->nr_bytes = 0;

	while (pmap->nr_bytes < FAST_PMAP_MAX_BYTES) {
		if (!buffer_size(buffer))
			goto partial;

		c = buffer_first_char(buffer);
		buffer_advance(buffer, 1);

		pmap->bytes[pmap->nr_bytes++] = c;

		if (c & 0x80)
			return 0;
	}

fail:
	return FAST_MSG_STATE_GARBLED;

partial:
	buffer_advance(buffer, -pmap->nr_bytes);

	if (data_read(buffer) > 0)
		goto retry;
	else
		goto fail;
}

static int fast_decode_uint(struct buffer *buffer, struct fast_pmap *pmap, struct fast_field *field)
{
	int ret = 0;
	i64 tmp;

	switch (field->op) {
	case FAST_OP_NONE:
		ret = parse_uint(buffer, &field->uint_value);

		if (ret)
			goto fail;

		field->state = FAST_STATE_ASSIGNED;

		if (field_is_mandatory(field))
			break;

		if (!field->uint_value)
			field->state = FAST_STATE_EMPTY;
		else
			field->uint_value--;

		break;
	case FAST_OP_COPY:
		if (!pmap_is_set(pmap, field->pmap_bit)) {
			switch (field->state) {
			case FAST_STATE_UNDEFINED:
				if (field_has_reset_value(field)) {
					field->state = FAST_STATE_ASSIGNED;
					field->uint_value = field->uint_reset;
				} else {
					if (field_is_mandatory(field)) {
						ret = FAST_MSG_STATE_GARBLED;
						goto fail;
					} else
						field->state = FAST_STATE_EMPTY;
				}

				break;
			case FAST_STATE_ASSIGNED:
				break;
			case FAST_STATE_EMPTY:
				if (field_is_mandatory(field)) {
					ret = FAST_MSG_STATE_GARBLED;
					goto fail;
				}

				break;
			default:
				break;
			}
		} else {
			ret = parse_uint(buffer, &field->uint_value);

			if (ret)
				goto fail;

			field->state = FAST_STATE_ASSIGNED;

			if (field_is_mandatory(field))
				break;

			if (!field->uint_value)
				field->state = FAST_STATE_EMPTY;
			else
				field->uint_value--;
		}

		break;
	case FAST_OP_INCR:
		if (!pmap_is_set(pmap, field->pmap_bit)) {
			switch (field->state) {
			case FAST_STATE_UNDEFINED:
				if (field_has_reset_value(field)) {
					field->state = FAST_STATE_ASSIGNED;
					field->uint_value = field->uint_reset;
				} else {
					if (field_is_mandatory(field)) {
						ret = FAST_MSG_STATE_GARBLED;
						goto fail;
					} else
						field->state = FAST_STATE_EMPTY;
				}

				break;
			case FAST_STATE_ASSIGNED:
				field->uint_value++;

				break;
			case FAST_STATE_EMPTY:
				if (field_is_mandatory(field)) {
					ret = FAST_MSG_STATE_GARBLED;
					goto fail;
				}

				break;
			default:
				break;
			}
		} else {
			ret = parse_uint(buffer, &field->uint_value);

			if (ret)
				goto fail;

			field->state = FAST_STATE_ASSIGNED;

			if (field_is_mandatory(field))
				break;

			if (!field->uint_value)
				field->state = FAST_STATE_EMPTY;
			else
				field->uint_value--;
		}

		break;
	case FAST_OP_DELTA:
		ret = parse_int(buffer, &tmp);

		if (ret)
			goto fail;

		field->state = FAST_STATE_ASSIGNED;

		if (tmp < 0)
			field->uint_value -= (-tmp);
		else
			field->uint_value += tmp;

		if (field_is_mandatory(field))
			break;

		if (!tmp)
			field->state = FAST_STATE_EMPTY;
		else if (tmp > 0)
			field->uint_value--;

		break;
	case FAST_OP_CONSTANT:
		if (field->state != FAST_STATE_ASSIGNED)
			field->uint_value = field->uint_reset;

		field->state = FAST_STATE_ASSIGNED;

		if (field_is_mandatory(field))
			break;

		if (!pmap_is_set(pmap, field->pmap_bit))
			field->state = FAST_STATE_EMPTY;

		break;
	default:
		return FAST_MSG_STATE_GARBLED;
	};

	return 0;

fail:
	return ret;
}

static int fast_decode_int(struct buffer *buffer, struct fast_pmap *pmap, struct fast_field *field)
{
	int ret = 0;
	i64 tmp;

	switch (field->op) {
	case FAST_OP_NONE:
		ret = parse_int(buffer, &field->int_value);

		if (ret)
			goto fail;

		field->state = FAST_STATE_ASSIGNED;

		if (field_is_mandatory(field))
			break;

		if (!field->int_value)
			field->state = FAST_STATE_EMPTY;
		else if (field->int_value > 0)
			field->int_value--;

		break;
	case FAST_OP_COPY:
		if (!pmap_is_set(pmap, field->pmap_bit)) {
			switch (field->state) {
			case FAST_STATE_UNDEFINED:
				if (field_has_reset_value(field)) {
					field->state = FAST_STATE_ASSIGNED;
					field->int_value = field->int_reset;
				} else {
					if (field_is_mandatory(field)) {
						ret = FAST_MSG_STATE_GARBLED;
						goto fail;
					} else
						field->state = FAST_STATE_EMPTY;
				}

				break;
			case FAST_STATE_ASSIGNED:
				break;
			case FAST_STATE_EMPTY:
				if (field_is_mandatory(field)) {
					ret = FAST_MSG_STATE_GARBLED;
					goto fail;
				}

				break;
			default:
				break;
			}
		} else {
			ret = parse_int(buffer, &field->int_value);

			if (ret)
				goto fail;

			field->state = FAST_STATE_ASSIGNED;

			if (field_is_mandatory(field))
				break;

			if (!field->int_value)
				field->state = FAST_STATE_EMPTY;
			else if (field->int_value > 0)
				field->int_value--;
		}

		break;
	case FAST_OP_INCR:
		if (!pmap_is_set(pmap, field->pmap_bit)) {
			switch (field->state) {
			case FAST_STATE_UNDEFINED:
				if (field_has_reset_value(field)) {
					field->state = FAST_STATE_ASSIGNED;
					field->int_value = field->int_reset;
				} else {
					if (field_is_mandatory(field)) {
						ret = FAST_MSG_STATE_GARBLED;
						goto fail;
					} else
						field->state = FAST_STATE_EMPTY;
				}

				break;
			case FAST_STATE_ASSIGNED:
				field->int_value++;

				break;
			case FAST_STATE_EMPTY:
				if (field_is_mandatory(field)) {
					ret = FAST_MSG_STATE_GARBLED;
					goto fail;
				}

				break;
			default:
				break;
			}
		} else {
			ret = parse_int(buffer, &field->int_value);

			if (ret)
				goto fail;

			field->state = FAST_STATE_ASSIGNED;

			if (field_is_mandatory(field))
				break;

			if (!field->int_value)
				field->state = FAST_STATE_EMPTY;
			else if (field->int_value > 0)
				field->int_value--;
		}

		break;
	case FAST_OP_DELTA:
		ret = parse_int(buffer, &tmp);

		if (ret)
			goto fail;

		field->state = FAST_STATE_ASSIGNED;
		field->int_value += tmp;

		if (field_is_mandatory(field))
			break;

		if (!tmp)
			field->state = FAST_STATE_EMPTY;
		else if (tmp > 0)
			field->int_value--;

		break;
	case FAST_OP_CONSTANT:
		if (field->state != FAST_STATE_ASSIGNED)
			field->int_value = field->int_reset;

		field->state = FAST_STATE_ASSIGNED;

		if (field_is_mandatory(field))
			break;

		if (!pmap_is_set(pmap, field->pmap_bit))
			field->state = FAST_STATE_EMPTY;

		break;
	default:
		return FAST_MSG_STATE_GARBLED;
	}

	return 0;

fail:
	return ret;
}

static int fast_decode_unicode(struct buffer *buffer, struct fast_pmap *pmap, struct fast_field *field)
{
	int ret;
	u64 len;

	switch (field->op) {
	case FAST_OP_NONE:
		ret = parse_uint(buffer, &len);
		if (ret)
			goto fail;

		field->state = FAST_STATE_ASSIGNED;

		if (!field_is_mandatory(field)) {
			if (!len) {
				field->state = FAST_STATE_EMPTY;
				break;
			} else
				len--;
		}

		ret = parse_bytes(buffer, field->string_value, len);
		if (ret)
			goto fail;

		memset(field->string_value + len, 0, sizeof(u64));

		break;
	case FAST_OP_COPY:
		if (!pmap_is_set(pmap, field->pmap_bit)) {
			switch (field->state) {
			case FAST_STATE_UNDEFINED:
				if (field_has_reset_value(field)) {
					field->state = FAST_STATE_ASSIGNED;
					memcpy(field->string_value, field->string_reset,
								strlen(field->string_reset) + 1);
				} else {
					if (field_is_mandatory(field)) {
						ret = FAST_MSG_STATE_GARBLED;
						goto fail;
					} else
						field->state = FAST_STATE_EMPTY;
				}

				break;
			case FAST_STATE_ASSIGNED:
				break;
			case FAST_STATE_EMPTY:
				if (field_is_mandatory(field)) {
					ret = FAST_MSG_STATE_GARBLED;
					goto fail;
				}

				break;
			default:
				break;
			}
		} else {
			ret = parse_uint(buffer, &len);

			if (ret)
				goto fail;

			field->state = FAST_STATE_ASSIGNED;

			if (!field_is_mandatory(field)) {
				if (!len) {
					field->state = FAST_STATE_EMPTY;
					break;
				} else
					len--;
			}

			ret = parse_bytes(buffer, field->string_value, len);
			if (ret)
				goto fail;

			memset(field->string_value + len, 0, sizeof(u64));
		}

		break;
	case FAST_OP_INCR:
			ret = FAST_MSG_STATE_GARBLED;
			goto fail;
	case FAST_OP_DELTA:
			ret = FAST_MSG_STATE_GARBLED;
			goto fail;
	case FAST_OP_CONSTANT:
		if (field->state != FAST_STATE_ASSIGNED)
			memcpy(field->string_value, field->string_reset,
						strlen(field->string_reset) + 1);

		field->state = FAST_STATE_ASSIGNED;

		if (field_is_mandatory(field))
			break;

		if (!pmap_is_set(pmap, field->pmap_bit))
			field->state = FAST_STATE_EMPTY;

		break;
	default:
		return FAST_MSG_STATE_GARBLED;
	}

	return 0;

fail:
	return ret;
}

static int fast_decode_ascii(struct buffer *buffer, struct fast_pmap *pmap, struct fast_field *field)
{
	int ret;

	switch (field->op) {
	case FAST_OP_NONE:
		ret = parse_string(buffer, field->string_value);

		if (ret < 0)
			goto fail;

		field->state = FAST_STATE_ASSIGNED;

		if (field_is_mandatory(field))
			break;

		if (ret == 1)
			field->state = FAST_STATE_EMPTY;

		break;
	case FAST_OP_COPY:
		if (!pmap_is_set(pmap, field->pmap_bit)) {
			switch (field->state) {
			case FAST_STATE_UNDEFINED:
				if (field_has_reset_value(field)) {
					field->state = FAST_STATE_ASSIGNED;
					memcpy(field->string_value, field->string_reset,
								strlen(field->string_reset) + 1);
				} else {
					if (field_is_mandatory(field)) {
						ret = FAST_MSG_STATE_GARBLED;
						goto fail;
					} else
						field->state = FAST_STATE_EMPTY;
				}

				break;
			case FAST_STATE_ASSIGNED:
				break;
			case FAST_STATE_EMPTY:
				if (field_is_mandatory(field)) {
					ret = FAST_MSG_STATE_GARBLED;
					goto fail;
				}

				break;
			default:
				break;
			}
		} else {
			ret = parse_string(buffer, field->string_value);

			if (ret < 0)
				goto fail;

			field->state = FAST_STATE_ASSIGNED;

			if (field_is_mandatory(field))
				break;

			if (ret == 1)
				field->state = FAST_STATE_EMPTY;
		}

		break;
	case FAST_OP_INCR:
			ret = FAST_MSG_STATE_GARBLED;
			goto fail;
	case FAST_OP_DELTA:
			ret = FAST_MSG_STATE_GARBLED;
			goto fail;
	case FAST_OP_CONSTANT:
		if (field->state != FAST_STATE_ASSIGNED)
			memcpy(field->string_value, field->string_reset,
						strlen(field->string_reset) + 1);

		field->state = FAST_STATE_ASSIGNED;

		if (field_is_mandatory(field))
			break;

		if (!pmap_is_set(pmap, field->pmap_bit))
			field->state = FAST_STATE_EMPTY;

		break;
	default:
		return FAST_MSG_STATE_GARBLED;
	}

	return 0;

fail:
	return ret;
}

static int fast_decode_string(struct buffer *buffer, struct fast_pmap *pmap, struct fast_field *field)
{
	if (field_has_flags(field, FAST_FIELD_FLAGS_UNICODE))
		return fast_decode_unicode(buffer, pmap, field);
	else
		return fast_decode_ascii(buffer, pmap, field);
}

static int fast_decode_decimal(struct buffer *buffer, struct fast_pmap *pmap, struct fast_field *field)
{
	int ret = 0;
	i64 exp, mnt;

	switch (field->op) {
	case FAST_OP_NONE:
		ret = parse_int(buffer, &exp);

		if (ret)
			goto fail;

		field->state = FAST_STATE_ASSIGNED;

		if (!field_is_mandatory(field)) {
			if (!exp) {
				field->state = FAST_STATE_EMPTY;
				break;
			} else if (exp > 0)
				exp--;
		}

		if (exp > 63 || exp < -63) {
			ret = FAST_MSG_STATE_GARBLED;
			goto fail;
		}

		ret = parse_int(buffer, &mnt);

		if (ret)
			goto fail;

		field->decimal_value.exp = exp;
		field->decimal_value.mnt = mnt;

		break;
	case FAST_OP_COPY:
		if (!pmap_is_set(pmap, field->pmap_bit)) {
			switch (field->state) {
			case FAST_STATE_UNDEFINED:
				if (field_has_reset_value(field)) {
					field->state = FAST_STATE_ASSIGNED;
					field->decimal_value.exp = field->decimal_reset.exp;
					field->decimal_value.mnt = field->decimal_reset.mnt;
				} else {
					if (field_is_mandatory(field)) {
						ret = FAST_MSG_STATE_GARBLED;
						goto fail;
					} else
						field->state = FAST_STATE_EMPTY;
				}

				break;
			case FAST_STATE_ASSIGNED:
				break;
			case FAST_STATE_EMPTY:
				if (field_is_mandatory(field)) {
					ret = FAST_MSG_STATE_GARBLED;
					goto fail;
				}

				break;
			default:
				break;
			}
		} else {
			ret = parse_int(buffer, &exp);

			if (ret)
				goto fail;

			field->state = FAST_STATE_ASSIGNED;

			if (!field_is_mandatory(field)) {
				if (!exp) {
					field->state = FAST_STATE_EMPTY;
					break;
				} else if (exp > 0)
					exp--;
			}

			if (exp > 63 || exp < -63) {
				ret = FAST_MSG_STATE_GARBLED;
				goto fail;
			}

			ret = parse_int(buffer, &mnt);

			if (ret)
				goto fail;

			field->decimal_value.exp = exp;
			field->decimal_value.mnt = mnt;
		}

		break;
	case FAST_OP_INCR:
		/* Do not applicable to decimal */
		ret = FAST_MSG_STATE_GARBLED;
		goto fail;
	case FAST_OP_DELTA:
		ret = parse_int(buffer, &exp);

		if (ret)
			goto fail;

		field->state = FAST_STATE_ASSIGNED;
		field->decimal_value.exp += exp;

		if (!field_is_mandatory(field)) {
			if (!exp) {
				field->state = FAST_STATE_EMPTY;
				break;
			} else if (exp > 0)
				field->decimal_value.exp--;
		}

		if (field->decimal_value.exp > 63 ||
			field->decimal_value.exp < -63) {
			ret = FAST_MSG_STATE_GARBLED;
			goto fail;
		}

		ret = parse_int(buffer, &mnt);

		if (ret)
			goto fail;

		field->decimal_value.mnt += mnt;

		break;
	case FAST_OP_CONSTANT:
		if (field->state != FAST_STATE_ASSIGNED) {
			field->decimal_value.exp = field->decimal_reset.exp;
			field->decimal_value.mnt = field->decimal_reset.mnt;
		}

		field->state = FAST_STATE_ASSIGNED;

		if (field_is_mandatory(field))
			break;

		if (!pmap_is_set(pmap, field->pmap_bit))
			field->state = FAST_STATE_EMPTY;

		break;
	default:
		return FAST_MSG_STATE_GARBLED;
	}

	return 0;

fail:
	return ret;
}

static int fast_decode_sequence(struct buffer *buffer, struct fast_pmap *pmap, struct fast_field *field)
{
	struct fast_sequence *seq;
	unsigned long nr_fields;
	struct fast_pmap spmap;
	struct fast_field *cur;
	int pmap_req;
	int ret = 0;
	int i, j;

	seq = field->ptr_value;

	ret = fast_decode_uint(buffer, pmap, &seq->length);

	if (ret)
		goto exit;

	if (field_state_empty(&seq->length)) {
		if (field_is_mandatory(field))
			ret = FAST_MSG_STATE_GARBLED;

		goto exit;
	}

	if (seq->length.uint_value >= FAST_SEQUENCE_ELEMENTS) {
		ret = FAST_MSG_STATE_GARBLED;
		goto exit;
	}

	pmap_req = field_has_flags(field, FAST_FIELD_FLAGS_PMAPREQ);

	for (i = 1; i <= seq->length.uint_value; i++) {
		if (pmap_req) {
			ret = parse_pmap(buffer, &spmap);

			if (ret)
				goto exit;
		}

		nr_fields = seq->elements->nr_fields;

		for (j = 0; j < nr_fields; j++) {
			cur = (seq->elements + i)->fields + j;
			field = seq->elements->fields + j;

			switch (field->type) {
			case FAST_TYPE_INT:
				ret = fast_decode_int(buffer, &spmap, field);
				if (ret)
					goto exit;

				cur->int_value = field->int_value;
				cur->state = field->state;
				break;
			case FAST_TYPE_UINT:
				ret = fast_decode_uint(buffer, &spmap, field);
				if (ret)
					goto exit;

				cur->uint_value = field->uint_value;
				cur->state = field->state;
				break;
			case FAST_TYPE_STRING:
				ret = fast_decode_string(buffer, &spmap, field);
				if (ret)
					goto exit;

				strcpy(cur->string_value, field->string_value);
				cur->state = field->state;
				break;
			case FAST_TYPE_DECIMAL:
				ret = fast_decode_decimal(buffer, &spmap, field);
				if (ret)
					goto exit;

				cur->decimal_value.exp = field->decimal_value.exp;
				cur->decimal_value.mnt = field->decimal_value.mnt;
				cur->state = field->state;
				break;
			case FAST_TYPE_SEQUENCE:
				/* At the moment we do no support nested sequences */
				ret = FAST_MSG_STATE_GARBLED;

				if (ret)
					goto exit;
				break;
			default:
				ret = FAST_MSG_STATE_GARBLED;
				goto exit;
			}
		}
	}

exit:
	return ret;
}

static inline struct fast_message *fast_get_msg(struct fast_message *msgs, int tid)
{
	int i;

	for (i = 0; i < FAST_TEMPLATE_MAX_NUMBER; i++) {
		if (msgs[i].tid == tid)
			return &msgs[i];
	}

	return NULL;
}

struct fast_message *fast_message_decode(struct fast_message *msgs, struct buffer *buffer, u64 last_tid)
{
	struct fast_message *msg;
	struct fast_field *field;
	struct fast_pmap pmap;
	unsigned long i;
	int ret;
	u64 tid;

	ret = parse_pmap(buffer, &pmap);
	if (ret)
		goto fail;

	if (pmap_is_set(&pmap, 0)) {
		ret = parse_uint(buffer, &tid);
		if (ret)
			goto fail;
	} else
		tid = last_tid;

	msg = fast_get_msg(msgs, tid);

	if (!msg)
		goto fail;

	msg->pmap = &pmap;

	for (i = 0; i < msg->nr_fields; i++) {
		field = msg->fields + i;

		switch (field->type) {
		case FAST_TYPE_INT:
			ret = fast_decode_int(buffer, msg->pmap, field);
			if (ret)
				goto fail;
			break;
		case FAST_TYPE_UINT:
			ret = fast_decode_uint(buffer, msg->pmap, field);
			if (ret)
				goto fail;
			break;
		case FAST_TYPE_STRING:
			ret = fast_decode_string(buffer, msg->pmap, field);
			if (ret)
				goto fail;
			break;
		case FAST_TYPE_DECIMAL:
			ret = fast_decode_decimal(buffer, msg->pmap, field);
			if (ret)
				goto fail;
			break;
		case FAST_TYPE_SEQUENCE:
			ret = fast_decode_sequence(buffer, msg->pmap, field);
			if (ret)
				goto fail;
			break;
		default:
			ret = FAST_MSG_STATE_GARBLED;
			goto fail;
		}
	}

	return msg;

fail:
	return NULL;
}

struct fast_message *fast_message_new(int nr_messages)
{
	struct fast_message *self = calloc(nr_messages, sizeof *self);

	if (!self)
		return NULL;

	return self;
}

void fast_fields_free(struct fast_message *self)
{
	struct fast_sequence *seq;
	struct fast_field *field;
	int i, j;

	if (!self)
		return;

	for (i = 0; i < self->nr_fields; i++) {
		field = self->fields + i;

		if (field->type == FAST_TYPE_SEQUENCE) {
			seq = field->ptr_value;

			for (j = 0; j < FAST_SEQUENCE_ELEMENTS; j++)
				free((seq->elements + j)->fields);

			free(field->ptr_value);
		}
	}

	free(self->fields);
}

void fast_message_free(struct fast_message *self, int nr_messages)
{
	int i;

	if (!self)
		return;

	for (i = 0; i < nr_messages; i++)
		fast_fields_free(self + i);

	free(self);

	return;
}

void fast_message_reset(struct fast_message *msg)
{
	struct fast_sequence *seq;
	struct fast_field *field;
	int i;

	for (i = 0; i < msg->nr_fields; i++) {
		field = msg->fields + i;

		switch (field->type) {
		case FAST_TYPE_INT:
			if (field->has_reset) {
				field->int_value = field->int_reset;
				field->int_previous = field->int_reset;
			} else {
				field->int_value = 0;
				field->int_previous = 0;
			}

			break;
		case FAST_TYPE_UINT:
			if (field->has_reset) {
				field->uint_value = field->uint_reset;
				field->uint_previous = field->uint_reset;
			} else {
				field->uint_value = 0;
				field->uint_previous = 0;
			}

			break;
		case FAST_TYPE_STRING:
			if (field->has_reset) {
				strcpy(field->string_value, field->string_reset);
				strcpy(field->string_previous, field->string_reset);
			} else {
				field->string_value[0] = 0;
				field->string_previous[0] = 0;
			}

			break;
		case FAST_TYPE_DECIMAL:
			if (field->has_reset) {
				field->decimal_value.exp = field->decimal_reset.exp;
				field->decimal_value.mnt = field->decimal_reset.mnt;
				field->decimal_previous.exp = field->decimal_reset.exp;
				field->decimal_previous.mnt = field->decimal_reset.mnt;
			} else {
				field->decimal_value.exp = 0;
				field->decimal_value.mnt = 0;
				field->decimal_previous.exp = 0;
				field->decimal_previous.mnt = 0;
			}

			break;
		case FAST_TYPE_SEQUENCE:
			seq = field->ptr_value;

			fast_message_reset(seq->elements);
			break;
		default:
			break;
		}
	}

	return;
}

static int transfer_int(struct buffer *buffer, i64 tmp)
{
	int size = transfer_size_int(tmp);

	if (buffer_remaining(buffer) < size)
		return -1;

	switch (size) {
	case 9:
		buffer_put(buffer, (tmp >> 56) & 0x7F);
	case 8:
		buffer_put(buffer, (tmp >> 49) & 0x7F);
	case 7:
		buffer_put(buffer, (tmp >> 42) & 0x7F);
	case 6:
		buffer_put(buffer, (tmp >> 35) & 0x7F);
	case 5:
		buffer_put(buffer, (tmp >> 28) & 0x7F);
	case 4:
		buffer_put(buffer, (tmp >> 21) & 0x7F);
	case 3:
		buffer_put(buffer, (tmp >> 14) & 0x7F);
	case 2:
		buffer_put(buffer, (tmp >> 7) & 0x7F);
	case 1:
		buffer_put(buffer, (tmp & 0x7F) | 0x80);
		break;
	default:
		return -1;
	};

	return 0;
}

static int fast_encode_int(struct buffer *buffer, struct fast_pmap *pmap, struct fast_field *field)
{
	i64 tmp = field->int_value;
	bool pset = true;

	switch (field->op) {
	case FAST_OP_NONE:
		pset = false;

		if (!field_is_mandatory(field)) {
			tmp = tmp >= 0 ? tmp + 1 : tmp;

			if (field_state_empty(field))
				goto empty;
		}

		field->state = FAST_STATE_ASSIGNED;

		goto transfer;
	case FAST_OP_COPY:
		if (!field_is_mandatory(field)) {
			tmp = tmp >= 0 ? tmp + 1 : tmp;

			if (field_state_empty(field))
				goto empty;
		}

		switch (field->state) {
		case FAST_STATE_UNDEFINED:
			field->state = FAST_STATE_ASSIGNED;
			goto transfer;
		case FAST_STATE_ASSIGNED:
			if (field_state_empty_previous(field))
				goto transfer;

			if (field->int_value != field->int_previous)
				goto transfer;

			break;
		case FAST_STATE_EMPTY:
			goto fail;
		default:
			goto fail;
		}

		break;
	case FAST_OP_INCR:
		if (!field_is_mandatory(field)) {
			tmp = tmp >= 0 ? tmp + 1 : tmp;

			if (field_state_empty(field))
				goto empty;
		}

		switch (field->state) {
		case FAST_STATE_UNDEFINED:
			field->state = FAST_STATE_ASSIGNED;
			goto transfer;
		case FAST_STATE_ASSIGNED:
			if (field_state_empty_previous(field))
				goto transfer;

			if (field->int_value != field->int_previous + 1)
				goto transfer;

			field->int_previous++;

			break;
		case FAST_STATE_EMPTY:
			goto fail;
		default:
			goto fail;
		}

		break;
	case FAST_OP_DELTA:
		tmp = field->int_value - field->int_previous;
		pset = false;

		if (!field_is_mandatory(field)) {
			tmp = tmp >= 0 ? tmp + 1 : tmp;

			if (field_state_empty(field))
				goto empty;
		}

		field->state = FAST_STATE_ASSIGNED;

		goto transfer;
	case FAST_OP_CONSTANT:
		if (!field_is_mandatory(field)) {
			if (!field_state_empty(field))
				pmap_set(pmap, field->pmap_bit);
			else
				break;
		}

		field->state = FAST_STATE_ASSIGNED;

		break;
	default:
		goto fail;
	};

	return 0;

empty:
	tmp = 0;
	field->int_value = field->int_previous;

transfer:
	field->int_previous = field->int_value;
	field->state_previous = field->state;

	if (transfer_int(buffer, tmp))
		goto fail;

	if (pset)
		pmap_set(pmap, field->pmap_bit);

	return 0;

fail:
	return -1;
}

static int transfer_uint(struct buffer *buffer, u64 tmp)
{
	int size = transfer_size_uint(tmp);

	if (buffer_remaining(buffer) < size)
		return -1;

	switch (size) {
	case 9:
		buffer_put(buffer, (tmp >> 56) & 0x7F);
	case 8:
		buffer_put(buffer, (tmp >> 49) & 0x7F);
	case 7:
		buffer_put(buffer, (tmp >> 42) & 0x7F);
	case 6:
		buffer_put(buffer, (tmp >> 35) & 0x7F);
	case 5:
		buffer_put(buffer, (tmp >> 28) & 0x7F);
	case 4:
		buffer_put(buffer, (tmp >> 21) & 0x7F);
	case 3:
		buffer_put(buffer, (tmp >> 14) & 0x7F);
	case 2:
		buffer_put(buffer, (tmp >> 7) & 0x7F);
	case 1:
		buffer_put(buffer, (tmp & 0x7F) | 0x80);
		break;
	default:
		return -1;
	};

	return 0;
}

static int fast_encode_uint(struct buffer *buffer, struct fast_pmap *pmap, struct fast_field *field)
{
	u64 tmp = field->uint_value;
	bool pset = true;
	i64 delta = 0;

	switch (field->op) {
	case FAST_OP_NONE:
		pset = false;

		if (!field_is_mandatory(field)) {
			tmp += 1;

			if (field_state_empty(field))
				goto empty;
		}

		field->state = FAST_STATE_ASSIGNED;

		goto transfer;
	case FAST_OP_COPY:
		if (!field_is_mandatory(field)) {
			tmp += 1;

			if (field_state_empty(field))
				goto empty;
		}

		switch (field->state) {
		case FAST_STATE_UNDEFINED:
			field->state = FAST_STATE_ASSIGNED;
			goto transfer;
		case FAST_STATE_ASSIGNED:
			if (field_state_empty_previous(field))
				goto transfer;

			if (field->uint_value != field->uint_previous)
				goto transfer;

			break;
		case FAST_STATE_EMPTY:
			goto fail;
		default:
			goto fail;
		}

		break;
	case FAST_OP_INCR:
		if (!field_is_mandatory(field)) {
			tmp += 1;

			if (field_state_empty(field))
				goto empty;
		}

		switch (field->state) {
		case FAST_STATE_UNDEFINED:
			field->state = FAST_STATE_ASSIGNED;
			goto transfer;
		case FAST_STATE_ASSIGNED:
			if (field_state_empty_previous(field))
				goto transfer;

			if (field->uint_value != field->uint_previous + 1)
				goto transfer;

			field->uint_previous++;

			break;
		case FAST_STATE_EMPTY:
			goto fail;
		default:
			goto fail;
		}

		break;
	case FAST_OP_DELTA:
		delta = field->uint_value - field->uint_previous;
		pset = false;

		if (!field_is_mandatory(field)) {
			delta = delta >= 0 ? delta + 1 : delta;

			if (field_state_empty(field))
				goto empty;
		}

		field->state = FAST_STATE_ASSIGNED;
		tmp = delta;

		goto transfer;
	case FAST_OP_CONSTANT:
		if (!field_is_mandatory(field)) {
			if (!field_state_empty(field))
				pmap_set(pmap, field->pmap_bit);
			else
				break;
		}

		field->state = FAST_STATE_ASSIGNED;

		break;
	default:
		goto fail;
	};

	return 0;

empty:
	tmp = 0;
	field->uint_value = field->uint_previous;

transfer:
	field->uint_previous = field->uint_value;
	field->state_previous = field->state;

	if (field->op != FAST_OP_DELTA) {
		if (transfer_uint(buffer, tmp))
			goto fail;
	} else {
		if (transfer_int(buffer, tmp))
			goto fail;
	}

	if (pset)
		pmap_set(pmap, field->pmap_bit);

	return 0;

fail:
	return -1;
}

static int transfer_string(struct buffer *buffer, char *tmp)
{
	int size;
	int i;

	if (!tmp)
		goto null;

	size = strlen(tmp);

	if (!size)
		size = 1;

	if (buffer_remaining(buffer) < size)
		goto fail;

	for (i = 0; i < size; i++)
		buffer_put(buffer, tmp[i]);

null:
	if (buffer_remaining(buffer) < 1)
		goto fail;

	buffer_put(buffer, 0x80);

	return 0;

fail:
	return -1;
}

static int fast_encode_string(struct buffer *buffer, struct fast_pmap *pmap, struct fast_field *field)
{
	char *tmp = field->string_value;
	bool pset = true;

	switch (field->op) {
	case FAST_OP_NONE:
		pset = false;

		if (!field_is_mandatory(field)) {
			if (field_state_empty(field))
				goto empty;
		}

		field->state = FAST_STATE_ASSIGNED;

		goto transfer;
	case FAST_OP_COPY:
		if (!field_is_mandatory(field)) {
			if (field_state_empty(field))
				goto empty;
		}

		switch (field->state) {
		case FAST_STATE_UNDEFINED:
			field->state = FAST_STATE_ASSIGNED;
			goto transfer;
		case FAST_STATE_ASSIGNED:
			if (field_state_empty_previous(field))
				goto transfer;

			if (strcmp(field->string_value, field->string_previous))
				goto transfer;

			break;
		case FAST_STATE_EMPTY:
			goto fail;
		default:
			goto fail;
		}

		break;
	case FAST_OP_INCR:
		goto fail;
	case FAST_OP_DELTA:
		goto fail;
	case FAST_OP_CONSTANT:
		if (!field_is_mandatory(field)) {
			if (!field_state_empty(field))
				pmap_set(pmap, field->pmap_bit);
			else
				break;
		}

		field->state = FAST_STATE_ASSIGNED;

		break;
	default:
		goto fail;
	};

	return 0;

empty:
	tmp = NULL;
	memcpy(field->string_value, field->string_previous, strlen(field->string_previous) + 1);

transfer:
	memcpy(field->string_previous, field->string_value, strlen(field->string_value) + 1);
	field->state_previous = field->state;

	if (transfer_string(buffer, tmp))
		goto fail;

	if (pset)
		pmap_set(pmap, field->pmap_bit);

	return 0;

fail:
	return -1;
}

static int fast_encode_decimal(struct buffer *buffer, struct fast_pmap *pmap, struct fast_field *field)
{
	i64 exp = field->decimal_value.exp;
	i64 mnt = field->decimal_value.mnt;

	bool pset = true;
	bool empty = false;

	switch (field->op) {
	case FAST_OP_NONE:
		pset = false;

		if (!field_is_mandatory(field)) {
			exp = exp >= 0 ? exp + 1 : exp;

			if (field_state_empty(field))
				goto empty;
		}

		field->state = FAST_STATE_ASSIGNED;

		goto transfer;
	case FAST_OP_COPY:
		if (!field_is_mandatory(field)) {
			exp = exp >= 0 ? exp + 1 : exp;

			if (field_state_empty(field))
				goto empty;
		}

		switch (field->state) {
		case FAST_STATE_UNDEFINED:
			field->state = FAST_STATE_ASSIGNED;
			goto transfer;
		case FAST_STATE_ASSIGNED:
			if (field_state_empty_previous(field))
				goto transfer;

			if ((field->decimal_value.exp != field->decimal_previous.exp) ||
				(field->decimal_value.mnt != field->decimal_previous.mnt))
				goto transfer;

			break;
		case FAST_STATE_EMPTY:
			goto fail;
		default:
			goto fail;
		}

		break;
	case FAST_OP_INCR:
		/* Not applicable to decimal */
		break;
	case FAST_OP_DELTA:
		exp = field->decimal_value.exp - field->decimal_previous.exp;
		mnt = field->decimal_value.mnt - field->decimal_previous.mnt;
		pset = false;

		if (!field_is_mandatory(field)) {
			exp = exp >= 0 ? exp + 1 : exp;

			if (field_state_empty(field))
				goto empty;
		}

		field->state = FAST_STATE_ASSIGNED;

		goto transfer;
	case FAST_OP_CONSTANT:
		if (!field_is_mandatory(field)) {
			if (!field_state_empty(field))
				pmap_set(pmap, field->pmap_bit);
			else
				break;
		}

		field->state = FAST_STATE_ASSIGNED;

		break;
	default:
		goto fail;
	};

	return 0;

empty:
	exp = 0;
	empty = true;

transfer:
	if (!empty) {
		field->decimal_previous.exp = field->decimal_value.exp;
		field->decimal_previous.mnt = field->decimal_value.mnt;
	}
	field->state_previous = field->state;

	if (transfer_int(buffer, exp))
		goto fail;

	if (!empty && transfer_int(buffer, mnt))
		goto fail;

	if (pset)
		pmap_set(pmap, field->pmap_bit);

	return 0;

fail:
	return -1;
}

int fast_message_encode(struct fast_message *msg)
{
	struct fast_field *field;
	struct fast_pmap pmap;
	int i;

	pmap.nr_bytes = FAST_PMAP_MAX_BYTES;
	memset(pmap.bytes, 0, pmap.nr_bytes);
	pmap_set(&pmap, 0);

	msg->pmap = &pmap;

	if (transfer_uint(msg->msg_buf, msg->tid))
		goto fail;

	for (i = 0; i < msg->nr_fields; i++) {
		field = msg->fields + i;

		switch (field->type) {
		case FAST_TYPE_INT:
			if (fast_encode_int(msg->msg_buf, msg->pmap, field))
				goto fail;
			break;
		case FAST_TYPE_UINT:
			if (fast_encode_uint(msg->msg_buf, msg->pmap, field))
				goto fail;
			break;
		case FAST_TYPE_STRING:
			if (fast_encode_string(msg->msg_buf, msg->pmap, field))
				goto fail;
			break;
		case FAST_TYPE_DECIMAL:
			if (fast_encode_decimal(msg->msg_buf, msg->pmap, field))
				goto fail;
			break;
		case FAST_TYPE_SEQUENCE:
			goto fail;
		default:
			goto fail;
		};
	}

	for (i = FAST_PMAP_MAX_BYTES; i > 0; i--) {
		if (pmap.bytes[i - 1])
			break;

		pmap.nr_bytes--;
	}

	if (buffer_remaining(msg->pmap_buf) < pmap.nr_bytes)
		goto fail;

	for (i = 0; i < pmap.nr_bytes - 1; i++)
		buffer_put(msg->pmap_buf, pmap.bytes[i] & 0x7F);

	buffer_put(msg->pmap_buf, pmap.bytes[pmap.nr_bytes - 1] | 0x80);

	return 0;

fail:
	return -1;
}

int fast_message_send(struct fast_message *self, int sockfd, int flags)
{
	struct iovec iov[2];
	int ret = 0;

	ret = fast_message_encode(self);
	if (ret)
		goto exit;

	buffer_to_iovec(self->pmap_buf, &iov[0]);
	buffer_to_iovec(self->msg_buf, &iov[1]);

	if (xwritev(sockfd, iov, ARRAY_SIZE(iov)) < 0) {
		ret = -1;
		goto exit;
	}

exit:
	self->pmap_buf = self->msg_buf = NULL;

	return ret;
}
