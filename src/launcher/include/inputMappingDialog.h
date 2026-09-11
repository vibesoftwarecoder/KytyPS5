#ifndef LAUNCHER_INCLUDE_INPUT_MAPPING_DIALOG_H_
#define LAUNCHER_INCLUDE_INPUT_MAPPING_DIALOG_H_

#include <QDialog>
#include <QMap>
#include <QString>
#include <QStringList>

class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QListWidget;
class QPushButton;
class QTimer;
class QTreeWidget;
class QTreeWidgetItem;
class QWidget;

// The "Controls" dialog: which controller plays the game, a live button test, controller button
// layout, and keyboard/mouse mapping.
class InputMappingDialog final: public QDialog {
public:
	InputMappingDialog(const QStringList& keyboard_mapping, const QStringList& controller_mapping,
	                   const QString& controller_selection, QWidget* parent = nullptr);
	~InputMappingDialog() override;

	[[nodiscard]] QStringList Mapping() const;
	[[nodiscard]] QStringList ControllerMapping() const;
	[[nodiscard]] QString     ControllerSelection() const;

private:
	struct Pad {
		void*   handle = nullptr; // SDL_GameController*
		QString name;
		int     type = 0; // SDL_GameControllerType
	};

	QWidget* CreateControllerTab(const QStringList& controller_mapping);
	QWidget* CreateKeyboardTab(const QStringList& keyboard_mapping);

	void               PollControllers();
	void               OpenController(int device_index);
	void               CloseController(int instance_id);
	void               RefreshControllerList();
	void               RefreshSelectionChoices(const QString& keep);
	void               OnControllerButton(int instance_id, int button);
	void               OnControllerAxis(int instance_id);
	void               RefreshPadButtonNames();
	void               UpdatePadModeLabel();
	[[nodiscard]] int  CurrentPadType() const;

	void SetKeyBinding(QTreeWidgetItem* item, const QString& binding);
	void SetPadBinding(QTreeWidgetItem* item, const QString& sdl_button);
	void ChangeKeyBinding();
	void ClearKeyBinding();
	void RestoreKeyDefaults();
	void ChangePadBinding();
	void ClearPadBinding();
	void RestorePadDefaults();
	void UpdateButtons();

	QMap<int, Pad> m_pads; // by SDL joystick instance id
	int            m_last_pad    = -1;
	bool           m_sdl_ready   = false;
	QTimer*        m_poll_timer  = nullptr;
	QDialog*       m_pad_capture = nullptr;
	QString        m_captured_pad_button;

	QListWidget* m_pad_list       = nullptr;
	QComboBox*   m_selection      = nullptr;
	QLabel*      m_test_label     = nullptr;
	QLabel*      m_axes_label     = nullptr;
	QLabel*      m_pad_mode_label = nullptr;
	QTreeWidget* m_pad_bindings   = nullptr;
	QPushButton* m_pad_change     = nullptr;
	QPushButton* m_pad_clear      = nullptr;
	bool         m_pad_custom     = false;

	QTreeWidget*    m_key_bindings = nullptr;
	QPushButton*    m_key_change   = nullptr;
	QPushButton*    m_key_clear    = nullptr;
	QDoubleSpinBox* m_sensitivity  = nullptr;
	bool            m_key_custom   = false;
};

#endif /* LAUNCHER_INCLUDE_INPUT_MAPPING_DIALOG_H_ */
