// Copyright 2016-2023 The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

#ifndef MUMBLE_MUMBLE_MANUALPLUGIN_H_
#define MUMBLE_MUMBLE_MANUALPLUGIN_H_

#include <QtCore/QtGlobal>
#include <QtWidgets/QDialog>
#include <QtWidgets/QGraphicsItem>
#include <QtWidgets/QGraphicsScene>

#include "LegacyPlugin.h"
#include "ui_ManualPlugin.h"
#include "ClientUser.h"

#include <atomic>
#include <chrono>
#include <mutex>
#include <qfiledialog.h>
#include <Qosc>
#include <QUdpSocket>
#include <QNetworkDatagram>

#define TEXT_OFFSET 4

struct Position2D {
	float x;
	float y;
};

// We need this typedef in order to be able to pass this hash as an argument
// to QMetaObject::invokeMethod
using PositionMap = QHash< unsigned int, Position2D >;
Q_DECLARE_METATYPE(PositionMap)

using PositionMap3D = QHash< unsigned int, Position3D >;
Q_DECLARE_METATYPE(PositionMap3D)
Q_DECLARE_METATYPE(float *)
Q_DECLARE_METATYPE(const ClientUser *)

/// A struct holding information about a stale entry in the
/// manual plugin's position window
struct StaleEntry {
	/// The time point since when this entry is considered stale
	std::chrono::time_point< std::chrono::steady_clock > staleSince;
	/// The pointer to the stale item
	QGraphicsItem *staleItem;
};

class Manual : public QDialog, public Ui::Manual {
	Q_OBJECT
public:
	Manual(QWidget *parent = 0);

	static void setSpeakerPositions(const QHash< unsigned int, Position2D > &positions);
	static void spatializeSpeakers(unsigned int, float *pos);
	void onUserAdded(mumble_connection_t connection, mumble_userid_t userID);
	void onUserRemoved(mumble_connection_t connection, mumble_userid_t userID);

	static std::vector< unsigned int > bufferToBeDeleted;
	static std::mutex bufferLock;
	static QString hrtfPath;
	static bool hrtfChanged;
	static bool isMono;

public slots:
	void on_qpbUnhinge_pressed();
	void on_qpbLinked_clicked(bool);
	void on_qpbActivated_clicked(bool);
	void on_qdsbX_valueChanged(double);
	void on_qdsbY_valueChanged(double);
	void on_qdsbZ_valueChanged(double);
	void on_qsbAzimuth_valueChanged(int);
	void on_qsbElevation_valueChanged(int);
	void on_qdAzimuth_valueChanged(int);
	void on_qdElevation_valueChanged(int);
	void on_qleContext_editingFinished();
	void on_qleIdentity_editingFinished();
	void on_OSC_IP_editingFinished();
	void on_OSC_Port_editingFinished();
	void on_buttonBox_clicked(QAbstractButton *);
	void on_qsbSilentUserDisplaytime_valueChanged(int);
	void on_select_HRTF_pressed();
	void on_preset_layout_combobox_currentIndexChanged(int);
	void on_Bottom_left_selector_currentIndexChanged(int);
	void on_Top_left_selector_currentIndexChanged(int);
	void on_Bottom_right_selector_currentIndexChanged(int);
	void on_Top_right_selector_currentIndexChanged(int);

	void on_bufferEntry(unsigned int id, float *pos);
	void on_speakerPositionUpdate(PositionMap positions);

	void on_updateStaleSpeakers();

	void receiveSocketMsg();

protected:
	QGraphicsScene *m_qgsScene;
	QGraphicsItem *m_qgiPosition;
	QGraphicsItem *selected_item;
	std::vector<QGraphicsItem *> user_positions;

	std::atomic< bool > updateLoopRunning;

	QHash< unsigned int, QGraphicsItem * > speakerPositions;
	QHash< unsigned int, StaleEntry > staleSpeakerPositions;
	QHash< unsigned int, Position3D > userPos;
	QHash< unsigned int, Position3D > *userPosition;
	QHash< QGraphicsItem *, unsigned int > userItem;
	QHash< QGraphicsItem *, QGraphicsTextItem * > userName;
	//add correlation between id and bufferpos

	bool eventFilter(QObject *, QEvent *);
	void changeEvent(QEvent *e);
	void updateTopAndFront(int orientation, int azimut);
	void createUserUI(const ClientUser *client);
	void deleteUserUI(mumble_userid_t userID);

	QHostAddress remoteAddr = QHostAddress("127.0.0.1");
	quint16 remotePort      = 0;
	QHostAddress localAddr  = QHostAddress("127.0.0.1");
	quint16 localPort       = 0;
	QUdpSocket OSCsocket;

	QRect viewport_rect;
	QRectF visible_scene_rect;

	void handleOSCmsg(const QOscMessage &msg);
	void handleOSCbundle(const QOscMessage &msg);

	int screenSpeakers[4];

	void screenSelectorIndexChanged(int uiId, int screenPositionIndex);
	void updateScreenPositions(int spatializationIndex);
};

MumblePlugin *ManualPlugin_getMumblePlugin();
MumblePluginQt *ManualPlugin_getMumblePluginQt();


/// A built-in "plugin" for positional data gathering allowing for manually placing the "players" in a UI
class ManualPlugin : public LegacyPlugin {
	friend class Plugin; // needed in order for Plugin::createNew to access LegacyPlugin::doInitialize()
private:
	Q_OBJECT
	Q_DISABLE_COPY(ManualPlugin)

protected:
	virtual void resolveFunctionPointers() Q_DECL_OVERRIDE;
	virtual void onUserAdded(mumble_connection_t connection, mumble_userid_t userID) const override;
	virtual void onUserRemoved(mumble_connection_t connection, mumble_userid_t userID) const override;
	ManualPlugin(QObject *p = nullptr);

public:
	virtual ~ManualPlugin() Q_DECL_OVERRIDE;
};

#endif
