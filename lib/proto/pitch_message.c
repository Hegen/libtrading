#include "libtrading/proto/pitch_message.h"

#include "libtrading/buffer.h"

#include <stdlib.h>
#include <string.h>

static unsigned long pitch_message_size(u8 type)
{
	switch (type) {
	case PITCH_MSG_ADD_ORDER_SHORT:
		return sizeof(struct pitch_msg_add_order_short);
	case PITCH_MSG_ADD_ORDER_LONG:
		return sizeof(struct pitch_msg_add_order_long);
	case PITCH_MSG_ORDER_EXECUTED:
		return sizeof(struct pitch_msg_order_executed);
	case PITCH_MSG_ORDER_CANCEL:
		return sizeof(struct pitch_msg_order_cancel);
	case PITCH_MSG_TRADE_SHORT:
		return sizeof(struct pitch_msg_trade_short);
	case PITCH_MSG_TRADE_LONG:
		return sizeof(struct pitch_msg_trade_long);
	case PITCH_MSG_TRADE_BREAK:
		return sizeof(struct pitch_msg_trade_break);
	case PITCH_MSG_TRADING_STATUS:
		return sizeof(struct pitch_msg_trading_status);
	case PITCH_MSG_AUCTION_UPDATE:
		return sizeof(struct pitch_msg_auction_update);
	case PITCH_MSG_AUCTION_SUMMARY:
		return sizeof(struct pitch_msg_auction_summary);
	default:
		break;
	};

	return 0;
}

int pitch_message_decode(struct buffer *buf, struct pitch_message *msg)
{
	size_t available;
	size_t size;
	u8 type;

	available = buffer_size(buf);

	if (!available)
		return -1;

	type = buffer_peek_8(buf);

	size = pitch_message_size(type);
	if (!size)
		return -1;

	if (available < size)
		return -1;

	memcpy(msg, buffer_start(buf), size);

	buffer_advance(buf, size);

	return 0;
}
