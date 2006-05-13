#include <stddef.h>
#include <string.h>
#include <vsprintf.h>
#include <errno.h>
#include <assert.h>
#include <byteswap.h>
#include <gpxe/scsi.h>
#include <gpxe/process.h>
#include <gpxe/iscsi.h>

/** @file
 *
 * iSCSI protocol
 *
 */

static void iscsi_start_tx ( struct iscsi_session *iscsi );

/****************************************************************************
 *
 * iSCSI SCSI command issuing
 *
 */

/**
 * Build iSCSI SCSI command BHS
 *
 * @v iscsi		iSCSI session
 */
static void iscsi_start_command ( struct iscsi_session *iscsi ) {
	struct iscsi_bhs_scsi_command *command = &iscsi->tx_bhs.scsi_command;

	assert ( ! ( iscsi->command->data_in && iscsi->command->data_out ) );

	/* Construct BHS and initiate transmission */
	iscsi_start_tx ( iscsi );
	command->opcode = ISCSI_OPCODE_SCSI_COMMAND;
	command->flags = ( ISCSI_FLAG_FINAL |
			   ISCSI_COMMAND_ATTR_SIMPLE );
	if ( iscsi->command->data_in )
		command->flags |= ISCSI_COMMAND_FLAG_READ;
	if ( iscsi->command->data_out )
		command->flags |= ISCSI_COMMAND_FLAG_WRITE;
	ISCSI_SET_LENGTHS ( command->lengths, 0, iscsi->command->data_out_len);
	command->lun = iscsi->lun;
	command->itt = htonl ( iscsi->itt );
	command->exp_len = htonl ( iscsi->command->data_in_len );
	memcpy ( &command->cdb, &iscsi->command->cdb, sizeof ( command->cdb ));
}

/**
 * Send iSCSI SCSI command data
 *
 * @v iscsi		iSCSI session
 */
static void iscsi_tx_command ( struct iscsi_session *iscsi ) {
	tcp_send ( &iscsi->tcp, iscsi->command->data_out + iscsi->tx_offset,
		   iscsi->command->data_out_len - iscsi->tx_offset );
}

/**
 * Receive data segment of an iSCSI data-in PDU
 *
 * @v iscsi		iSCSI session
 * @v data		Received data
 * @v len		Length of received data
 * @v remaining		Data remaining after this data
 * 
 */
static void iscsi_rx_data_in ( struct iscsi_session *iscsi, void *data,
			       size_t len, size_t remaining ) {
	struct iscsi_bhs_data_in *data_in = &iscsi->rx_bhs.data_in;
	unsigned long offset;

	/* Copy data to data-in buffer */
	offset = ntohl ( data_in->offset ) + iscsi->rx_offset;
	assert ( iscsi->command != NULL );
	assert ( iscsi->command->data_in != NULL );
	assert ( ( offset + len ) <= iscsi->command->data_in_len );
	memcpy ( ( iscsi->command->data_in + offset ), data, len );

	/* If this is the end, flag as complete */
	if ( ( data_in->flags & ISCSI_FLAG_FINAL ) && ( remaining == 0 ) )
		iscsi->status |= ISCSI_STATUS_DONE;
}

/****************************************************************************
 *
 * iSCSI login
 *
 */

/**
 * Build iSCSI login request strings
 *
 * @v iscsi		iSCSI session
 *
 * These are the initial set of strings sent in the first login
 * request PDU.
 */
static int iscsi_build_login_request_strings ( struct iscsi_session *iscsi,
					       void *data, size_t len ) {
	return snprintf ( data, len,
			  "InitiatorName=%s%c"
			  "TargetName=%s%c"
			  "MaxRecvDataSegmentLength=512%c"
			  "SessionType=Normal%c"
			  "DataDigest=None%c"
			  "HeaderDigest=None%c",
			  iscsi->initiator, 0, iscsi->target, 0,
			  0, 0, 0, 0 );
}

/**
 * Build iSCSI login request BHS
 *
 * @v iscsi		iSCSI session
 * @v first		Login request is the first request of a session
 */
static void iscsi_start_login ( struct iscsi_session *iscsi, int first ) {
	struct iscsi_bhs_login_request *request = &iscsi->tx_bhs.login_request;
	int len;

	/* Construct BHS and initiate transmission */
	iscsi_start_tx ( iscsi );
	request->opcode = ( ISCSI_OPCODE_LOGIN_REQUEST |
			    ISCSI_FLAG_IMMEDIATE );
	request->flags = ( ISCSI_LOGIN_FLAG_TRANSITION |
			   ISCSI_LOGIN_CSG_OPERATIONAL_NEGOTIATION |
			   ISCSI_LOGIN_NSG_FULL_FEATURE_PHASE );
	/* version_max and version_min left as zero */
	if ( first ) {
		len = iscsi_build_login_request_strings ( iscsi, NULL, 0 );
		ISCSI_SET_LENGTHS ( request->lengths, 0, len );
	}
	request->isid_iana_en = htonl ( ISCSI_ISID_IANA |
					IANA_EN_FEN_SYSTEMS );
	/* isid_iana_qual left as zero */
	request->tsih = htons ( iscsi->tsih );
	/* itt left as zero */
	/* cid left as zero */
}

/**
 * Transmit data segment of an iSCSI login request PDU
 *
 * @v iscsi		iSCSI session
 *
 * For login requests, the data segment consists of the login strings.
 */
static void iscsi_tx_login_request ( struct iscsi_session *iscsi ) {
	int len;

	len = iscsi_build_login_request_strings ( iscsi, tcp_buffer,
						  tcp_buflen );
	tcp_send ( &iscsi->tcp, tcp_buffer + iscsi->tx_offset,
		   len - iscsi->tx_offset );
}

/**
 * Receive data segment of an iSCSI login response PDU
 *
 * @v iscsi		iSCSI session
 * @v data		Received data
 * @v len		Length of received data
 * @v remaining		Data remaining after this data
 * 
 */
static void iscsi_rx_login_response ( struct iscsi_session *iscsi,
				      void *data __unused,
				      size_t len __unused,
				      size_t remaining __unused ) {
	struct iscsi_bhs_login_response *response
		= &iscsi->rx_bhs.login_response;

	/* Check for fatal errors */
	if ( response->status_class != 0 ) {
		printf ( "iSCSI login failure: class %02x detail %02x\n",
			 response->status_class, response->status_detail );
		iscsi->status |= ( ISCSI_STATUS_DONE | ISCSI_STATUS_ERR );
		tcp_close ( &iscsi->tcp );
		return;
	}

	/* If server did not transition, send back another login
	 * request without any login strings.
	 */
	if ( ! ( response->flags & ISCSI_LOGIN_FLAG_TRANSITION ) ) {
		iscsi_start_login ( iscsi, 0 );
		return;
	}

	/* Record TSIH for future reference */
	iscsi->tsih = ntohl ( response->tsih );
	
	/* Send the SCSI command */
	iscsi_start_command ( iscsi );
}

/****************************************************************************
 *
 * iSCSI to TCP interface
 *
 */

static inline struct iscsi_session *
tcp_to_iscsi ( struct tcp_connection *conn ) {
	return container_of ( conn, struct iscsi_session, tcp );
}

/**
 * Start up a new TX PDU
 *
 * @v iscsi		iSCSI session
 *
 * This initiates the process of sending a new PDU.  Only one PDU may
 * be in transit at any one time.
 */
static void iscsi_start_tx ( struct iscsi_session *iscsi ) {
	assert ( iscsi->tx_state == ISCSI_TX_IDLE );
	
	/* Initialise TX BHS */
	memset ( &iscsi->tx_bhs, 0, sizeof ( iscsi->tx_bhs ) );
	iscsi->tx_bhs.common_request.cmdsn = htonl ( iscsi->cmdsn );
	iscsi->tx_bhs.common_request.expstatsn = htonl ( iscsi->statsn + 1 );

	/* Flag TX engine to start transmitting */
	iscsi->tx_state = ISCSI_TX_BHS;
	iscsi->tx_offset = 0;
}

/**
 * Transmit data segment of an iSCSI PDU
 *
 * @v iscsi		iSCSI session
 * 
 * Handle transmission of part of a PDU data segment.  iscsi::tx_bhs
 * will be valid when this is called.
 */
static void iscsi_tx_data ( struct iscsi_session *iscsi ) {
	struct iscsi_bhs_common *common = &iscsi->tx_bhs.common;

	switch ( common->opcode & ISCSI_OPCODE_MASK ) {
	case ISCSI_OPCODE_SCSI_COMMAND:
		iscsi_tx_command ( iscsi );
		break;
	case ISCSI_OPCODE_LOGIN_REQUEST:
		iscsi_tx_login_request ( iscsi );
		break;
	default:
		assert ( 0 );
		break;
	}
}

/**
 * Handle TCP ACKs
 *
 * @v iscsi		iSCSI session
 * 
 * Updates iscsi->tx_offset and, if applicable, transitions to the
 * next TX state.
 */
static void iscsi_acked ( struct tcp_connection *conn, size_t len ) {
	struct iscsi_session *iscsi = tcp_to_iscsi ( conn );
	struct iscsi_bhs_common *common = &iscsi->tx_bhs.common;
	size_t max_tx_offset;
	enum iscsi_tx_state next_state;
	
	iscsi->tx_offset += len;
	while ( 1 ) {
		switch ( iscsi->tx_state ) {
		case ISCSI_TX_BHS:
			max_tx_offset = sizeof ( iscsi->tx_bhs );
			next_state = ISCSI_TX_AHS;
			break;
		case ISCSI_TX_AHS:
			max_tx_offset = 4 * ISCSI_AHS_LEN ( common->lengths );
			next_state = ISCSI_TX_DATA;
			break;
		case ISCSI_TX_DATA:
			max_tx_offset = ISCSI_DATA_LEN ( common->lengths );
			next_state = ISCSI_TX_DATA_PADDING;
			break;
		case ISCSI_TX_DATA_PADDING:
			max_tx_offset = ISCSI_DATA_PAD_LEN ( common->lengths );
			next_state = ISCSI_TX_IDLE;
			break;
		case ISCSI_TX_IDLE:
			return;
		default:
			assert ( 0 );
			return;
		}
		assert ( iscsi->tx_offset <= max_tx_offset );

		/* If the whole of the current portion has not yet
		 * been acked, stay in this state for now.
		 */
		if ( iscsi->tx_offset != max_tx_offset )
			return;
		
		iscsi->tx_state = next_state;
		iscsi->tx_offset = 0;
	}
}

/**
 * Transmit iSCSI PDU
 *
 * @v iscsi		iSCSI session
 * 
 * Constructs data to be sent for the current TX state
 */
static void iscsi_senddata ( struct tcp_connection *conn ) {
	struct iscsi_session *iscsi = tcp_to_iscsi ( conn );
	struct iscsi_bhs_common *common = &iscsi->tx_bhs.common;
	static const char pad[] = { '\0', '\0', '\0' };

	switch ( iscsi->tx_state ) {
	case ISCSI_TX_IDLE:
		/* Nothing to send */
		break;
	case ISCSI_TX_BHS:
		tcp_send ( conn, &iscsi->tx_bhs.bytes[iscsi->tx_offset],
			   ( sizeof ( iscsi->tx_bhs ) - iscsi->tx_offset ) );
		break;
	case ISCSI_TX_AHS:
		/* We don't yet have an AHS transmission mechanism */
		assert ( 0 );
		break;
	case ISCSI_TX_DATA:
		iscsi_tx_data ( iscsi );
		break;
	case ISCSI_TX_DATA_PADDING:
		tcp_send ( conn, pad, ( ISCSI_DATA_PAD_LEN ( common->lengths )
					- iscsi->tx_offset ) );
		break;
	default:
		assert ( 0 );
		break;
	}
}

/**
 * Receive data segment of an iSCSI PDU
 *
 * @v iscsi		iSCSI session
 * @v data		Received data
 * @v len		Length of received data
 * @v remaining		Data remaining after this data
 *
 * Handle processing of part of a PDU data segment.  iscsi::rx_bhs
 * will be valid when this is called.
 */
static void iscsi_rx_data ( struct iscsi_session *iscsi, void *data,
			    size_t len, size_t remaining ) {
	struct iscsi_bhs_common_response *response
		= &iscsi->rx_bhs.common_response;

	/* Update cmdsn and statsn */
	iscsi->cmdsn = ntohl ( response->expcmdsn );
	iscsi->statsn = ntohl ( response->statsn );

	/* Increment itt when we receive a final response */
	if ( response->flags & ISCSI_FLAG_FINAL )
		iscsi->itt++;

	switch ( response->opcode & ISCSI_OPCODE_MASK ) {
	case ISCSI_OPCODE_LOGIN_RESPONSE:
		iscsi_rx_login_response ( iscsi, data, len, remaining );
		break;
	case ISCSI_OPCODE_DATA_IN:
		iscsi_rx_data_in ( iscsi, data, len, remaining );
		break;
	default:
		printf ( "Unknown iSCSI opcode %02x\n", response->opcode );
		iscsi->status |= ( ISCSI_STATUS_DONE | ISCSI_STATUS_ERR );
		break;
	}
}

/**
 * Discard portion of an iSCSI PDU.
 *
 * @v iscsi		iSCSI session
 * @v data		Received data
 * @v len		Length of received data
 * @v remaining		Data remaining after this data
 *
 * This discards data from a portion of a received PDU.
 */
static void iscsi_rx_discard ( struct iscsi_session *iscsi __unused,
			       void *data __unused, size_t len __unused,
			       size_t remaining __unused ) {
	/* Do nothing */
}

/**
 * Receive basic header segment of an iSCSI PDU
 *
 * @v iscsi		iSCSI session
 * @v data		Received data
 * @v len		Length of received data
 * @v remaining		Data remaining after this data
 *
 * This fills in iscsi::rx_bhs with the data from the BHS portion of
 * the received PDU.
 */
static void iscsi_rx_bhs ( struct iscsi_session *iscsi, void *data,
			   size_t len, size_t remaining __unused ) {
	memcpy ( &iscsi->rx_bhs.bytes[iscsi->rx_offset], data, len );
}

/**
 * Receive new data
 *
 * @v tcp		TCP connection
 * @v data		Received data
 * @v len		Length of received data
 *
 * This handles received PDUs.  The receive strategy is to fill in
 * iscsi::rx_bhs with the contents of the BHS portion of the PDU,
 * throw away any AHS portion, and then process each part of the data
 * portion as it arrives.  The data processing routine therefore
 * always has a full copy of the BHS available, even for portions of
 * the data in different packets to the BHS.
 */
static void iscsi_newdata ( struct tcp_connection *conn, void *data,
			    size_t len ) {
	struct iscsi_session *iscsi = tcp_to_iscsi ( conn );
	struct iscsi_bhs_common *common = &iscsi->rx_bhs.common;
	void ( *process ) ( struct iscsi_session *iscsi, void *data,
			    size_t len, size_t remaining );
	size_t max_rx_offset;
	enum iscsi_rx_state next_state;
	size_t frag_len;
	size_t remaining;

	while ( 1 ) {
		switch ( iscsi->rx_state ) {
		case ISCSI_RX_BHS:
			process = iscsi_rx_bhs;
			max_rx_offset = sizeof ( iscsi->rx_bhs );
			next_state = ISCSI_RX_AHS;			
			break;
		case ISCSI_RX_AHS:
			process = iscsi_rx_discard;
			max_rx_offset = 4 * ISCSI_AHS_LEN ( common->lengths );
			next_state = ISCSI_RX_DATA;
			break;
		case ISCSI_RX_DATA:
			process = iscsi_rx_data;
			max_rx_offset = ISCSI_DATA_LEN ( common->lengths );
			next_state = ISCSI_RX_DATA_PADDING;
			break;
		case ISCSI_RX_DATA_PADDING:
			process = iscsi_rx_discard;
			max_rx_offset = ISCSI_DATA_PAD_LEN ( common->lengths );
			next_state = ISCSI_RX_BHS;
			break;
		default:
			assert ( 0 );
			return;
		}

		frag_len = max_rx_offset - iscsi->rx_offset;
		if ( frag_len > len )
			frag_len = len;
		remaining = max_rx_offset - iscsi->rx_offset - frag_len;
		process ( iscsi, data, frag_len, remaining );

		iscsi->rx_offset += frag_len;
		data += frag_len;
		len -= frag_len;

		/* If all the data for this state has not yet been
		 * received, stay in this state for now.
		 */
		if ( iscsi->rx_offset != max_rx_offset )
			return;

		iscsi->rx_state = next_state;
		iscsi->rx_offset = 0;
	}
}

/**
 * Handle TCP connection closure
 *
 * @v conn		TCP connection
 * @v status		Error code, if any
 *
 */
static void iscsi_closed ( struct tcp_connection *conn, int status __unused ) {
	struct iscsi_session *iscsi = tcp_to_iscsi ( conn );

	/* Clear connected flag */
	iscsi->status &= ~ISCSI_STATUS_CONNECTED;

	/* Retry connection if within the retry limit, otherwise fail */
	if ( ++iscsi->retry_count <= ISCSI_MAX_RETRIES ) {
		tcp_connect ( conn );
	} else {
		iscsi->status |= ( ISCSI_STATUS_DONE | ISCSI_STATUS_ERR );
	}
}

/**
 * Handle TCP connection opening
 *
 * @v conn		TCP connection
 *
 */
static void iscsi_connected ( struct tcp_connection *conn ) {
	struct iscsi_session *iscsi = tcp_to_iscsi ( conn );

	/* Set connected flag and reset retry count */
	iscsi->status |= ISCSI_STATUS_CONNECTED;
	iscsi->retry_count = 0;

	/* Prepare to receive PDUs. */
	iscsi->rx_state = ISCSI_RX_BHS;
	iscsi->rx_offset = 0;

	/* Start logging in */
	iscsi_start_login ( iscsi, 1 );
}

/** iSCSI TCP operations */
static struct tcp_operations iscsi_tcp_operations = {
	.closed		= iscsi_closed,
	.connected	= iscsi_connected,
	.acked		= iscsi_acked,
	.newdata	= iscsi_newdata,
	.senddata	= iscsi_senddata,
};

/**
 * Issue SCSI command via iSCSI session
 *
 * @v iscsi		iSCSI session
 * @v command		SCSI command
 * @ret rc		Return status code
 */
static int iscsi_command ( struct iscsi_session *iscsi,
			   struct scsi_command *command ) {
	iscsi->command = command;
	iscsi->status &= ~( ISCSI_STATUS_DONE | ISCSI_STATUS_ERR );

	if ( iscsi->status & ISCSI_STATUS_CONNECTED ) {
		iscsi_start_command ( iscsi );
	} else {
		iscsi->tcp.tcp_op = &iscsi_tcp_operations;
		tcp_connect ( &iscsi->tcp );
	}

	while ( ! ( iscsi->status & ISCSI_STATUS_DONE ) ) {
		step();
	}

	iscsi->command = NULL;

	return ( ( iscsi->status & ISCSI_STATUS_ERR ) ? -EIO : 0 );	
}

/**
 * Issue SCSI command via iSCSI device
 *
 * @v scsi		SCSI device
 * @v command		SCSI command
 * @ret rc		Return status code
 */
static int iscsi_scsi_command ( struct scsi_device *scsi,
				struct scsi_command *command ) {
	struct iscsi_device *iscsidev
		= container_of ( scsi, struct iscsi_device, scsi );

	return iscsi_command ( &iscsidev->iscsi, command );
}

/**
 * Initialise iSCSI device
 *
 * @v iscsidev		iSCSI device
 */
int init_iscsidev ( struct iscsi_device *iscsidev ) {
	iscsidev->scsi.command = iscsi_scsi_command;
	iscsidev->scsi.lun = iscsidev->iscsi.lun;
	return init_scsidev ( &iscsidev->scsi );
}