// SPDX-License-Identifier: GPL-2.0-only
/*
 * Qualcomm QMI LOC GNSS receiver driver
 *
 * The QMI LOC service runs on a remote processor and is discovered over
 * QRTR. It provides NMEA data without requiring a modem character device.
 */

#include <linux/errno.h>
#include <linux/gnss.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/net.h>
#include <linux/platform_device.h>
#include <linux/qrtr.h>
#include <linux/string.h>
#include <linux/soc/qcom/qmi.h>

#define QMI_LOC_SERVICE_ID			0x10
#define QMI_LOC_SERVICE_VERSION			2
#define QMI_LOC_SERVICE_INSTANCE		0

#define QMI_LOC_REGISTER_EVENTS			0x0021
#define QMI_LOC_START				0x0022
#define QMI_LOC_STOP				0x0023
#define QMI_LOC_EVENT_NMEA			0x0026

#define QMI_LOC_EVENT_MASK_NMEA			BIT_ULL(2)
#define QMI_LOC_SESSION_ID			1

#define QMI_LOC_CLIENT_ID_MAX			4
#define QMI_LOC_CLIENT_TYPE_NFW			2

#define QMI_LOC_FIX_RECURRENCE_PERIODIC		1
#define QMI_LOC_INTERMEDIATE_REPORTS_ON		1
#define QMI_LOC_MIN_INTERVAL_MS			1000

#define QMI_LOC_NMEA_MAX			200
#define QMI_LOC_EXPANDED_NMEA_MAX		4095
#define QMI_LOC_NMEA_IND_MAX_MSG_LEN		4301

#define QMI_LOC_REGISTER_EVENTS_MAX_MSG_LEN	29
#define QMI_LOC_START_MAX_MSG_LEN		25
#define QMI_LOC_STOP_MAX_MSG_LEN			4

#define QMI_ERR_INVALID_ARG_V01			48

struct qmi_loc_generic_resp {
	struct qmi_response_type_v01 resp;
};

static const struct qmi_elem_info qmi_loc_generic_resp_ei[] = {
	{
		.data_type = QMI_STRUCT,
		.elem_len = 1,
		.elem_size = sizeof(struct qmi_response_type_v01),
		.array_type = NO_ARRAY,
		.tlv_type = 0x02,
		.offset = offsetof(struct qmi_loc_generic_resp, resp),
		.ei_array = qmi_response_type_v01_ei,
	},
	{}
};

struct qmi_loc_register_events_req {
	u64 event_mask;

	u8 client_id_valid;
	char client_id[QMI_LOC_CLIENT_ID_MAX + 1];

	u8 client_type_valid;
	u32 client_type;

	u8 request_notification_valid;
	u8 request_notification;
};

static const struct qmi_elem_info qmi_loc_register_events_req_ei[] = {
	{
		.data_type = QMI_UNSIGNED_8_BYTE,
		.elem_len = 1,
		.elem_size = sizeof(u64),
		.array_type = NO_ARRAY,
		.tlv_type = 0x01,
		.offset = offsetof(struct qmi_loc_register_events_req, event_mask),
	},
	{
		.data_type = QMI_OPT_FLAG,
		.elem_len = 1,
		.elem_size = sizeof(u8),
		.array_type = NO_ARRAY,
		.tlv_type = 0x10,
		.offset = offsetof(struct qmi_loc_register_events_req,
				   client_id_valid),
	},
	{
		.data_type = QMI_STRING,
		.elem_len = QMI_LOC_CLIENT_ID_MAX + 1,
		.elem_size = sizeof(char),
		.array_type = NO_ARRAY,
		.tlv_type = 0x10,
		.offset = offsetof(struct qmi_loc_register_events_req, client_id),
	},
	{
		.data_type = QMI_OPT_FLAG,
		.elem_len = 1,
		.elem_size = sizeof(u8),
		.array_type = NO_ARRAY,
		.tlv_type = 0x11,
		.offset = offsetof(struct qmi_loc_register_events_req,
				   client_type_valid),
	},
	{
		.data_type = QMI_UNSIGNED_4_BYTE,
		.elem_len = 1,
		.elem_size = sizeof(u32),
		.array_type = NO_ARRAY,
		.tlv_type = 0x11,
		.offset = offsetof(struct qmi_loc_register_events_req, client_type),
	},
	{
		.data_type = QMI_OPT_FLAG,
		.elem_len = 1,
		.elem_size = sizeof(u8),
		.array_type = NO_ARRAY,
		.tlv_type = 0x12,
		.offset = offsetof(struct qmi_loc_register_events_req,
				   request_notification_valid),
	},
	{
		.data_type = QMI_UNSIGNED_1_BYTE,
		.elem_len = 1,
		.elem_size = sizeof(u8),
		.array_type = NO_ARRAY,
		.tlv_type = 0x12,
		.offset = offsetof(struct qmi_loc_register_events_req,
				   request_notification),
	},
	{}
};

struct qmi_loc_start_req {
	u8 session_id;

	u8 recurrence_valid;
	u32 recurrence;

	u8 intermediate_reports_valid;
	u32 intermediate_reports;

	u8 min_interval_valid;
	u32 min_interval;
};

static const struct qmi_elem_info qmi_loc_start_req_ei[] = {
	{
		.data_type = QMI_UNSIGNED_1_BYTE,
		.elem_len = 1,
		.elem_size = sizeof(u8),
		.array_type = NO_ARRAY,
		.tlv_type = 0x01,
		.offset = offsetof(struct qmi_loc_start_req, session_id),
	},
	{
		.data_type = QMI_OPT_FLAG,
		.elem_len = 1,
		.elem_size = sizeof(u8),
		.array_type = NO_ARRAY,
		.tlv_type = 0x10,
		.offset = offsetof(struct qmi_loc_start_req, recurrence_valid),
	},
	{
		.data_type = QMI_UNSIGNED_4_BYTE,
		.elem_len = 1,
		.elem_size = sizeof(u32),
		.array_type = NO_ARRAY,
		.tlv_type = 0x10,
		.offset = offsetof(struct qmi_loc_start_req, recurrence),
	},
	{
		.data_type = QMI_OPT_FLAG,
		.elem_len = 1,
		.elem_size = sizeof(u8),
		.array_type = NO_ARRAY,
		.tlv_type = 0x12,
		.offset = offsetof(struct qmi_loc_start_req,
				   intermediate_reports_valid),
	},
	{
		.data_type = QMI_UNSIGNED_4_BYTE,
		.elem_len = 1,
		.elem_size = sizeof(u32),
		.array_type = NO_ARRAY,
		.tlv_type = 0x12,
		.offset = offsetof(struct qmi_loc_start_req, intermediate_reports),
	},
	{
		.data_type = QMI_OPT_FLAG,
		.elem_len = 1,
		.elem_size = sizeof(u8),
		.array_type = NO_ARRAY,
		.tlv_type = 0x13,
		.offset = offsetof(struct qmi_loc_start_req, min_interval_valid),
	},
	{
		.data_type = QMI_UNSIGNED_4_BYTE,
		.elem_len = 1,
		.elem_size = sizeof(u32),
		.array_type = NO_ARRAY,
		.tlv_type = 0x13,
		.offset = offsetof(struct qmi_loc_start_req, min_interval),
	},
	{}
};

struct qmi_loc_stop_req {
	u8 session_id;
};

static const struct qmi_elem_info qmi_loc_stop_req_ei[] = {
	{
		.data_type = QMI_UNSIGNED_1_BYTE,
		.elem_len = 1,
		.elem_size = sizeof(u8),
		.array_type = NO_ARRAY,
		.tlv_type = 0x01,
		.offset = offsetof(struct qmi_loc_stop_req, session_id),
	},
	{}
};

struct qmi_loc_nmea_ind {
	char nmea[QMI_LOC_NMEA_MAX + 1];

	u8 expanded_nmea_valid;
	char expanded_nmea[QMI_LOC_EXPANDED_NMEA_MAX + 1];
};

static const struct qmi_elem_info qmi_loc_nmea_ind_ei[] = {
	{
		.data_type = QMI_STRING,
		.elem_len = QMI_LOC_NMEA_MAX + 1,
		.elem_size = sizeof(char),
		.array_type = NO_ARRAY,
		.tlv_type = 0x01,
		.offset = offsetof(struct qmi_loc_nmea_ind, nmea),
	},
	{
		.data_type = QMI_OPT_FLAG,
		.elem_len = 1,
		.elem_size = sizeof(u8),
		.array_type = NO_ARRAY,
		.tlv_type = 0x10,
		.offset = offsetof(struct qmi_loc_nmea_ind, expanded_nmea_valid),
	},
	{
		.data_type = QMI_STRING,
		.elem_len = QMI_LOC_EXPANDED_NMEA_MAX + 1,
		.elem_size = sizeof(char),
		.array_type = NO_ARRAY,
		.tlv_type = 0x10,
		.offset = offsetof(struct qmi_loc_nmea_ind, expanded_nmea),
	},
	{}
};

struct qcom_qmi_loc {
	struct device *dev;
	struct qmi_handle qmi;
	struct gnss_device *gdev;
	struct mutex rx_lock; /* serializes running state and RX */
	bool running;
	bool removing;
};

static void qcom_qmi_loc_set_running(struct qcom_qmi_loc *loc, bool running)
{
	mutex_lock(&loc->rx_lock);
	loc->running = running;
	mutex_unlock(&loc->rx_lock);
}

static int qcom_qmi_loc_request(struct qcom_qmi_loc *loc, u16 message_id,
				size_t max_len, const struct qmi_elem_info *ei,
				const void *request, u16 *qmi_error)
{
	struct qmi_loc_generic_resp response = {};
	struct qmi_txn txn;
	int ret;

	ret = qmi_txn_init(&loc->qmi, &txn, qmi_loc_generic_resp_ei,
			   &response);
	if (ret < 0)
		return ret;

	ret = qmi_send_request(&loc->qmi, NULL, &txn, message_id, max_len,
			       ei, request);
	if (ret < 0) {
		qmi_txn_cancel(&txn);
		return ret;
	}

	ret = qmi_txn_wait(&txn, 5 * HZ);
	if (ret < 0)
		return ret;

	if (qmi_error)
		*qmi_error = response.resp.error;

	if (response.resp.result != QMI_RESULT_SUCCESS_V01) {
		dev_dbg(loc->dev, "QMI request %#x failed: error %u\n",
			message_id, response.resp.error);
		return -EREMOTEIO;
	}

	return 0;
}

static bool qcom_qmi_loc_can_use_legacy_registration(u16 error)
{
	return error == QMI_ERR_MALFORMED_MSG_V01 ||
	       error == QMI_ERR_INVALID_ARG_V01 ||
	       error == QMI_ERR_ENCODING_V01 ||
	       error == QMI_ERR_NOT_SUPPORTED_V01;
}

static int qcom_qmi_loc_register_events(struct qcom_qmi_loc *loc)
{
	struct qmi_loc_register_events_req request = {
		.event_mask = QMI_LOC_EVENT_MASK_NMEA,
		.client_id_valid = true,
		.client_id = "LINX",
		.client_type_valid = true,
		.client_type = QMI_LOC_CLIENT_TYPE_NFW,
		.request_notification_valid = true,
		.request_notification = false,
	};
	u16 qmi_error = 0;
	int ret;

	ret = qcom_qmi_loc_request(loc, QMI_LOC_REGISTER_EVENTS,
				   QMI_LOC_REGISTER_EVENTS_MAX_MSG_LEN,
				   qmi_loc_register_events_req_ei, &request,
				   &qmi_error);
	if (!ret || !qcom_qmi_loc_can_use_legacy_registration(qmi_error))
		return ret;

	dev_dbg(loc->dev,
		"client identification rejected (QMI error %u), retrying legacy registration\n",
		qmi_error);

	request.client_id_valid = false;
	request.client_type_valid = false;
	request.request_notification_valid = false;

	return qcom_qmi_loc_request(loc, QMI_LOC_REGISTER_EVENTS,
				    QMI_LOC_REGISTER_EVENTS_MAX_MSG_LEN,
				    qmi_loc_register_events_req_ei, &request,
				    NULL);
}

static int qcom_qmi_loc_start(struct qcom_qmi_loc *loc)
{
	const struct qmi_loc_start_req request = {
		.session_id = QMI_LOC_SESSION_ID,
		.recurrence_valid = true,
		.recurrence = QMI_LOC_FIX_RECURRENCE_PERIODIC,
		.intermediate_reports_valid = true,
		.intermediate_reports = QMI_LOC_INTERMEDIATE_REPORTS_ON,
		.min_interval_valid = true,
		.min_interval = QMI_LOC_MIN_INTERVAL_MS,
	};

	return qcom_qmi_loc_request(loc, QMI_LOC_START,
				    QMI_LOC_START_MAX_MSG_LEN,
				    qmi_loc_start_req_ei, &request, NULL);
}

static int qcom_qmi_loc_stop(struct qcom_qmi_loc *loc)
{
	const struct qmi_loc_stop_req request = {
		.session_id = QMI_LOC_SESSION_ID,
	};

	return qcom_qmi_loc_request(loc, QMI_LOC_STOP,
				    QMI_LOC_STOP_MAX_MSG_LEN,
				    qmi_loc_stop_req_ei, &request, NULL);
}

static int qcom_qmi_loc_open(struct gnss_device *gdev)
{
	struct qcom_qmi_loc *loc = gnss_get_drvdata(gdev);
	int ret;

	ret = qcom_qmi_loc_register_events(loc);
	if (ret) {
		dev_err(loc->dev, "failed to register NMEA events: %d\n", ret);
		return ret;
	}

	qcom_qmi_loc_set_running(loc, true);
	ret = qcom_qmi_loc_start(loc);
	if (ret) {
		qcom_qmi_loc_set_running(loc, false);
		dev_err(loc->dev, "failed to start GNSS session: %d\n", ret);
		return ret;
	}

	dev_dbg(loc->dev, "GNSS session started\n");

	return 0;
}

static void qcom_qmi_loc_close(struct gnss_device *gdev)
{
	struct qcom_qmi_loc *loc = gnss_get_drvdata(gdev);
	int ret;

	qcom_qmi_loc_set_running(loc, false);
	if (READ_ONCE(loc->removing))
		return;

	ret = qcom_qmi_loc_stop(loc);
	if (ret)
		dev_warn(loc->dev, "failed to stop GNSS session: %d\n", ret);
	else
		dev_dbg(loc->dev, "GNSS session stopped\n");
}

static const struct gnss_operations qcom_qmi_loc_gnss_ops = {
	.open = qcom_qmi_loc_open,
	.close = qcom_qmi_loc_close,
};

static void qcom_qmi_loc_nmea_ind(struct qmi_handle *qmi,
				  struct sockaddr_qrtr *sq,
				  struct qmi_txn *txn, const void *data)
{
	const struct qmi_loc_nmea_ind *indication = data;
	struct qcom_qmi_loc *loc = container_of(qmi, struct qcom_qmi_loc, qmi);
	const char *nmea;
	size_t len;
	int inserted;

	if (indication->expanded_nmea_valid && indication->expanded_nmea[0]) {
		nmea = indication->expanded_nmea;
		len = strnlen(nmea, QMI_LOC_EXPANDED_NMEA_MAX);
	} else {
		nmea = indication->nmea;
		len = strnlen(nmea, QMI_LOC_NMEA_MAX);
	}

	if (!len)
		return;

	mutex_lock(&loc->rx_lock);
	if (!loc->running) {
		mutex_unlock(&loc->rx_lock);
		return;
	}

	inserted = gnss_insert_raw(loc->gdev,
				   (const unsigned char *)nmea, len);
	mutex_unlock(&loc->rx_lock);

	if (inserted != len)
		dev_warn_ratelimited(loc->dev, "dropped %zu bytes of NMEA data\n",
				     len - inserted);
}

static const struct qmi_msg_handler qcom_qmi_loc_handlers[] = {
	{
		.type = QMI_INDICATION,
		.msg_id = QMI_LOC_EVENT_NMEA,
		.ei = qmi_loc_nmea_ind_ei,
		.decoded_size = sizeof(struct qmi_loc_nmea_ind),
		.fn = qcom_qmi_loc_nmea_ind,
	},
	{}
};

static int qcom_qmi_loc_probe(struct platform_device *pdev)
{
	struct sockaddr_qrtr *sq = dev_get_platdata(&pdev->dev);
	struct qcom_qmi_loc *loc;
	int ret;

	loc = devm_kzalloc(&pdev->dev, sizeof(*loc), GFP_KERNEL);
	if (!loc)
		return -ENOMEM;

	loc->dev = &pdev->dev;
	mutex_init(&loc->rx_lock);

	ret = qmi_handle_init(&loc->qmi, QMI_LOC_NMEA_IND_MAX_MSG_LEN, NULL,
			      qcom_qmi_loc_handlers);
	if (ret)
		return ret;

	ret = kernel_connect(loc->qmi.sock, (struct sockaddr_unsized *)sq,
			     sizeof(*sq), 0);
	if (ret) {
		dev_err(&pdev->dev, "failed to connect to QMI LOC service: %d\n",
			ret);
		goto err_release_qmi;
	}

	loc->gdev = gnss_allocate_device(&pdev->dev);
	if (!loc->gdev) {
		ret = -ENOMEM;
		goto err_release_qmi;
	}

	loc->gdev->type = GNSS_TYPE_NMEA;
	loc->gdev->ops = &qcom_qmi_loc_gnss_ops;
	gnss_set_drvdata(loc->gdev, loc);

	ret = gnss_register_device(loc->gdev);
	if (ret)
		goto err_put_gnss;

	platform_set_drvdata(pdev, loc);

	return 0;

err_put_gnss:
	gnss_put_device(loc->gdev);
err_release_qmi:
	qmi_handle_release(&loc->qmi);

	return ret;
}

static void qcom_qmi_loc_remove(struct platform_device *pdev)
{
	struct qcom_qmi_loc *loc = platform_get_drvdata(pdev);

	WRITE_ONCE(loc->removing, true);
	gnss_deregister_device(loc->gdev);
	qmi_handle_release(&loc->qmi);
	gnss_put_device(loc->gdev);
}

static struct platform_driver qcom_qmi_loc_driver = {
	.probe = qcom_qmi_loc_probe,
	.remove = qcom_qmi_loc_remove,
	.driver = {
		.name = "qcom-qmi-loc",
	},
};

static int qcom_qmi_loc_new_server(struct qmi_handle *qmi,
				   struct qmi_service *service)
{
	struct sockaddr_qrtr sq = {
		.sq_family = AF_QIPCRTR,
		.sq_node = service->node,
		.sq_port = service->port,
	};
	struct platform_device *pdev;
	int ret;

	pdev = platform_device_alloc("qcom-qmi-loc", PLATFORM_DEVID_AUTO);
	if (!pdev)
		return -ENOMEM;

	ret = platform_device_add_data(pdev, &sq, sizeof(sq));
	if (ret)
		goto err_put_device;

	ret = platform_device_add(pdev);
	if (ret)
		goto err_put_device;

	service->priv = pdev;

	return 0;

err_put_device:
	platform_device_put(pdev);

	return ret;
}

static void qcom_qmi_loc_del_server(struct qmi_handle *qmi,
				    struct qmi_service *service)
{
	platform_device_unregister(service->priv);
}

static const struct qmi_ops qcom_qmi_loc_lookup_ops = {
	.new_server = qcom_qmi_loc_new_server,
	.del_server = qcom_qmi_loc_del_server,
};

static struct qmi_handle qcom_qmi_loc_lookup;

static int __init qcom_qmi_loc_init(void)
{
	int ret;

	ret = platform_driver_register(&qcom_qmi_loc_driver);
	if (ret)
		return ret;

	ret = qmi_handle_init(&qcom_qmi_loc_lookup, 0,
			      &qcom_qmi_loc_lookup_ops, NULL);
	if (ret)
		goto err_unregister_driver;

	ret = qmi_add_lookup(&qcom_qmi_loc_lookup, QMI_LOC_SERVICE_ID,
			     QMI_LOC_SERVICE_VERSION,
			     QMI_LOC_SERVICE_INSTANCE);
	if (ret)
		goto err_release_lookup;

	return 0;

err_release_lookup:
	qmi_handle_release(&qcom_qmi_loc_lookup);
err_unregister_driver:
	platform_driver_unregister(&qcom_qmi_loc_driver);

	return ret;
}

static void __exit qcom_qmi_loc_exit(void)
{
	qmi_handle_release(&qcom_qmi_loc_lookup);
	platform_driver_unregister(&qcom_qmi_loc_driver);
}

module_init(qcom_qmi_loc_init);
module_exit(qcom_qmi_loc_exit);

MODULE_DESCRIPTION("Qualcomm QMI LOC GNSS receiver driver");
MODULE_LICENSE("GPL");
