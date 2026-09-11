#include "inputMappingDialog.h"

#include "SDL.h"

#include <QAbstractItemView>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFont>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHash>
#include <QHeaderView>
#include <QKeyEvent>
#include <QKeySequence>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPushButton>
#include <QSignalBlocker>
#include <QTabWidget>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <cstdlib>
#include <utility>

namespace {

constexpr int  BINDING_COLUMN            = 1;
constexpr int  CONTROL_INDEX_ROLE        = Qt::UserRole + 1;
constexpr auto DEFAULT_MOUSE_SENSITIVITY = 1.0;
constexpr char MOUSE_SENSITIVITY[]       = "MouseSensitivity=";
constexpr int  POLL_INTERVAL_MS          = 16;
constexpr int  AXIS_TEST_THRESHOLD       = 8000;
constexpr char SELECTION_FIRST[]         = "first";
constexpr char SELECTION_LAST[]          = "last";
constexpr char SELECTION_NAME_PREFIX[]   = "name:";

enum class Group { DPad, LeftStick, RightStick, Face, Shoulders, Clicks, System };

struct PadControl {
	const char* id;
	const char* label;
	Group       group;
	const char* default_key;
	const char* default_pad_button; // SDL game controller button name; empty when analog only
	const char* analog_source;      // analog input that always drives this control, if any
};

constexpr PadControl PAD_CONTROLS[] = {
    {"Up", "D-pad Up", Group::DPad, "Up", "dpup", ""},
    {"Down", "D-pad Down", Group::DPad, "Down", "dpdown", ""},
    {"Left", "D-pad Left", Group::DPad, "Left", "dpleft", ""},
    {"Right", "D-pad Right", Group::DPad, "Right", "dpright", ""},
    {"LeftStickUp", "Left stick Up", Group::LeftStick, "W", "", "Left stick"},
    {"LeftStickDown", "Left stick Down", Group::LeftStick, "S", "", "Left stick"},
    {"LeftStickLeft", "Left stick Left", Group::LeftStick, "A", "", "Left stick"},
    {"LeftStickRight", "Left stick Right", Group::LeftStick, "D", "", "Left stick"},
    {"RightStickUp", "Right stick Up", Group::RightStick, "T", "", "Right stick"},
    {"RightStickDown", "Right stick Down", Group::RightStick, "G", "", "Right stick"},
    {"RightStickLeft", "Right stick Left", Group::RightStick, "F", "", "Right stick"},
    {"RightStickRight", "Right stick Right", Group::RightStick, "H", "", "Right stick"},
    {"Cross", "✕ Cross", Group::Face, "J", "a", ""},
    {"Circle", "○ Circle", Group::Face, "L", "b", ""},
    {"Square", "□ Square", Group::Face, "K", "x", ""},
    {"Triangle", "△ Triangle", Group::Face, "I", "y", ""},
    {"L1", "L1", Group::Shoulders, "Q", "leftshoulder", ""},
    {"R1", "R1", Group::Shoulders, "E", "rightshoulder", ""},
    {"L2", "L2", Group::Shoulders, "", "", "Left trigger"},
    {"R2", "R2", Group::Shoulders, "", "", "Right trigger"},
    {"L3", "L3 (press left stick)", Group::Clicks, "Left Shift", "leftstick", ""},
    {"R3", "R3 (press right stick)", Group::Clicks, "Left Ctrl", "rightstick", ""},
    {"Options", "Options", Group::System, "Return", "start", ""},
    {"TouchPad", "Touch pad left (SELECT)", Group::System, "Backspace", "touchpad", ""},
    {"TouchPadRight", "Touch pad right (START)", Group::System, "Tab", "", ""},
};

struct ButtonNames {
	const char* sdl;
	const char* playstation;
	const char* xbox;
};

constexpr ButtonNames BUTTON_NAMES[] = {
    {"a", "✕ Cross", "A"},
    {"b", "○ Circle", "B"},
    {"x", "□ Square", "X"},
    {"y", "△ Triangle", "Y"},
    {"back", "Share / Create", "View"},
    {"guide", "PS button", "Xbox button"},
    {"start", "Options", "Menu"},
    {"leftstick", "L3 (left stick click)", "Left stick click"},
    {"rightstick", "R3 (right stick click)", "Right stick click"},
    {"leftshoulder", "L1", "LB"},
    {"rightshoulder", "R1", "RB"},
    {"dpup", "D-pad Up", "D-pad Up"},
    {"dpdown", "D-pad Down", "D-pad Down"},
    {"dpleft", "D-pad Left", "D-pad Left"},
    {"dpright", "D-pad Right", "D-pad Right"},
    {"misc1", "Mute", "Share"},
    {"paddle1", "Paddle 1", "Paddle 1"},
    {"paddle2", "Paddle 2", "Paddle 2"},
    {"paddle3", "Paddle 3", "Paddle 3"},
    {"paddle4", "Paddle 4", "Paddle 4"},
    {"touchpad", "Touch pad click", "Touch pad click"},
};

QString GroupName(Group group) {
	switch (group) {
		case Group::DPad: return QObject::tr("D-pad");
		case Group::LeftStick: return QObject::tr("Left stick");
		case Group::RightStick: return QObject::tr("Right stick");
		case Group::Face: return QObject::tr("Face buttons");
		case Group::Shoulders: return QObject::tr("Shoulder buttons and triggers");
		case Group::Clicks: return QObject::tr("Stick clicks");
		case Group::System: return QObject::tr("System");
	}
	return {};
}

bool IsPlayStation(int type) {
	return type == SDL_CONTROLLER_TYPE_PS3 || type == SDL_CONTROLLER_TYPE_PS4 ||
	       type == SDL_CONTROLLER_TYPE_PS5;
}

QString ControllerTypeName(int type) {
	switch (type) {
		case SDL_CONTROLLER_TYPE_PS3: return QStringLiteral("PlayStation 3");
		case SDL_CONTROLLER_TYPE_PS4: return QStringLiteral("PlayStation 4");
		case SDL_CONTROLLER_TYPE_PS5: return QStringLiteral("PlayStation 5");
		case SDL_CONTROLLER_TYPE_XBOX360: return QStringLiteral("Xbox 360");
		case SDL_CONTROLLER_TYPE_XBOXONE: return QStringLiteral("Xbox");
		case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_PRO: return QStringLiteral("Switch Pro");
		default: return QObject::tr("standard layout");
	}
}

// The name printed on the controller for an SDL button: PlayStation glyphs on PlayStation
// controllers, Xbox labels on everything else (SDL's standard layout is Xbox-shaped).
QString PadButtonName(const QString& sdl_button, int type) {
	for (const auto& names: BUTTON_NAMES) {
		if (sdl_button == QLatin1String(names.sdl)) {
			return QString::fromUtf8(IsPlayStation(type) ? names.playstation : names.xbox);
		}
	}
	return sdl_button;
}

const PadControl& ControlOf(const QTreeWidgetItem* item) {
	return PAD_CONTROLS[item->data(0, CONTROL_INDEX_ROLE).toInt()];
}

bool IsBindingItem(const QTreeWidgetItem* item) {
	return item != nullptr && item->parent() != nullptr;
}

QList<QTreeWidgetItem*> BindingItems(const QTreeWidget* tree) {
	QList<QTreeWidgetItem*> items;
	for (int group = 0; group < tree->topLevelItemCount(); group++) {
		const auto* group_item = tree->topLevelItem(group);
		for (int child = 0; child < group_item->childCount(); child++) {
			items.append(group_item->child(child));
		}
	}
	return items;
}

// One row per PlayStation control, under bold group headings.
QTreeWidget* CreateBindingTree(QWidget* parent, const QString& input_header) {
	auto* tree = new QTreeWidget(parent);
	tree->setColumnCount(2);
	tree->setHeaderLabels({QObject::tr("PlayStation control"), input_header});
	tree->setRootIsDecorated(false);
	tree->setIndentation(14);
	tree->setSelectionMode(QAbstractItemView::SingleSelection);
	tree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
	tree->header()->setSectionResizeMode(1, QHeaderView::Stretch);

	QTreeWidgetItem* group_item = nullptr;
	int              last_group = -1;
	for (int index = 0; index < static_cast<int>(std::size(PAD_CONTROLS)); index++) {
		const auto& control = PAD_CONTROLS[index];
		if (static_cast<int>(control.group) != last_group) {
			last_group = static_cast<int>(control.group);
			group_item = new QTreeWidgetItem(tree);
			group_item->setText(0, GroupName(control.group));
			group_item->setFlags(Qt::ItemIsEnabled);
			group_item->setFirstColumnSpanned(true);
			auto font = group_item->font(0);
			font.setBold(true);
			group_item->setFont(0, font);
		}
		auto* item = new QTreeWidgetItem(group_item);
		item->setText(0, QObject::tr(control.label));
		item->setData(0, Qt::UserRole, QString::fromLatin1(control.id));
		item->setData(0, CONTROL_INDEX_ROLE, index);
	}
	tree->expandAll();
	tree->setItemsExpandable(false);
	return tree;
}

QString KeypadName(int key) {
	if (key >= Qt::Key_0 && key <= Qt::Key_9) {
		return QStringLiteral("Keypad %1").arg(key - Qt::Key_0);
	}

	switch (key) {
		case Qt::Key_Return:
		case Qt::Key_Enter: return QStringLiteral("Keypad Enter");
		case Qt::Key_Slash: return QStringLiteral("Keypad /");
		case Qt::Key_Asterisk: return QStringLiteral("Keypad *");
		case Qt::Key_Minus: return QStringLiteral("Keypad -");
		case Qt::Key_Plus: return QStringLiteral("Keypad +");
		case Qt::Key_Period: return QStringLiteral("Keypad .");
		case Qt::Key_Equal: return QStringLiteral("Keypad =");
		case Qt::Key_Comma: return QStringLiteral("Keypad ,");
		default: return {};
	}
}

QString KeyName(const QKeyEvent& event) {
	const int key = event.key();
	if (event.modifiers().testFlag(Qt::KeypadModifier)) {
		return KeypadName(key);
	}
	if (key >= Qt::Key_A && key <= Qt::Key_Z) {
		return QChar(key);
	}
	if (key >= Qt::Key_0 && key <= Qt::Key_9) {
		return QChar(key);
	}
	if (key >= Qt::Key_F1 && key <= Qt::Key_F24) {
		return QStringLiteral("F%1").arg(key - Qt::Key_F1 + 1);
	}

	switch (key) {
		case Qt::Key_Return:
		case Qt::Key_Enter: return QStringLiteral("Return");
		case Qt::Key_Backspace: return QStringLiteral("Backspace");
		case Qt::Key_Tab: return QStringLiteral("Tab");
		case Qt::Key_Shift: return QStringLiteral("Left Shift");
		case Qt::Key_Control: return QStringLiteral("Left Ctrl");
		case Qt::Key_Alt: return QStringLiteral("Left Alt");
		case Qt::Key_Meta: return QStringLiteral("Left GUI");
		case Qt::Key_Insert: return QStringLiteral("Insert");
		case Qt::Key_Delete: return QStringLiteral("Delete");
		case Qt::Key_Home: return QStringLiteral("Home");
		case Qt::Key_End: return QStringLiteral("End");
		case Qt::Key_PageUp: return QStringLiteral("PageUp");
		case Qt::Key_PageDown: return QStringLiteral("PageDown");
		case Qt::Key_Left: return QStringLiteral("Left");
		case Qt::Key_Right: return QStringLiteral("Right");
		case Qt::Key_Up: return QStringLiteral("Up");
		case Qt::Key_Down: return QStringLiteral("Down");
		case Qt::Key_CapsLock: return QStringLiteral("CapsLock");
		case Qt::Key_NumLock: return QStringLiteral("Numlock");
		case Qt::Key_ScrollLock: return QStringLiteral("ScrollLock");
		case Qt::Key_Pause: return QStringLiteral("Pause");
		case Qt::Key_Print: return QStringLiteral("PrintScreen");
		default: break;
	}

	if (event.modifiers() != Qt::NoModifier) {
		return {};
	}
	const auto name = QKeySequence(key).toString(QKeySequence::PortableText);
	return name.size() == 1 ? name : QString();
}

class InputCaptureDialog final: public QDialog {
public:
	explicit InputCaptureDialog(QWidget* parent): QDialog(parent) {
		setWindowTitle(tr("Set Key"));
		setModal(true);
		setMinimumWidth(360);

		auto* layout = new QVBoxLayout(this);
		m_label      = new QLabel(
		    tr("Press a key or mouse button.\nSpace, F1, F7, and F11 are reserved; Esc cancels."),
		    this);
		m_label->setAlignment(Qt::AlignCenter);
		layout->addWidget(m_label);
	}

	[[nodiscard]] const QString& Binding() const { return m_binding; }

protected:
	void keyPressEvent(QKeyEvent* event) override {
		if (event->isAutoRepeat()) {
			return;
		}
		if (event->key() == Qt::Key_Escape) {
			reject();
			return;
		}
		if (event->key() == Qt::Key_Space || event->key() == Qt::Key_F1 ||
		    event->key() == Qt::Key_F7 || event->key() == Qt::Key_F11) {
			m_label->setText(tr("That key is reserved by the emulator."));
			return;
		}

		m_binding = KeyName(*event);
		if (!m_binding.isEmpty()) {
			accept();
		} else {
			m_label->setText(tr("That key is not supported."));
		}
	}

	void mousePressEvent(QMouseEvent* event) override {
		switch (event->button()) {
			case Qt::LeftButton: m_binding = QStringLiteral("Mouse:Left"); break;
			case Qt::RightButton: m_binding = QStringLiteral("Mouse:Right"); break;
			case Qt::MiddleButton: m_binding = QStringLiteral("Mouse:Middle"); break;
			case Qt::BackButton: m_binding = QStringLiteral("Mouse:X1"); break;
			case Qt::ForwardButton: m_binding = QStringLiteral("Mouse:X2"); break;
			default: return;
		}
		accept();
	}

private:
	QLabel* m_label = nullptr;
	QString m_binding;
};

QHash<QString, QString> ParseMapping(const QStringList& mapping) {
	QHash<QString, QString> result;
	for (const auto& entry: mapping) {
		if (entry.startsWith(QLatin1String(MOUSE_SENSITIVITY))) {
			continue;
		}
		const auto separator = entry.indexOf(QLatin1Char('='));
		if (separator > 0 && separator + 1 < entry.size()) {
			const auto binding = entry.mid(separator + 1);
			for (auto item = result.begin(); item != result.end();) {
				if (item.value().compare(binding, Qt::CaseInsensitive) == 0) {
					item = result.erase(item);
				} else {
					++item;
				}
			}
			result.insert(entry.left(separator), binding);
		}
	}
	return result;
}

double ParseMouseSensitivity(const QStringList& mapping) {
	for (const auto& entry: mapping) {
		if (entry.startsWith(QLatin1String(MOUSE_SENSITIVITY))) {
			return entry.mid(sizeof(MOUSE_SENSITIVITY) - 1).toDouble();
		}
	}
	return DEFAULT_MOUSE_SENSITIVITY;
}

} // namespace

InputMappingDialog::InputMappingDialog(const QStringList& keyboard_mapping,
                                       const QStringList& controller_mapping,
                                       const QString& controller_selection, QWidget* parent)
    : QDialog(parent) {
	setWindowTitle(tr("Controls"));
	resize(640, 800);

	auto* layout = new QVBoxLayout(this);
	auto* tabs   = new QTabWidget(this);
	tabs->addTab(CreateControllerTab(controller_mapping), tr("Controller"));
	tabs->addTab(CreateKeyboardTab(keyboard_mapping), tr("Keyboard and mouse"));
	layout->addWidget(tabs);

	auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
	layout->addWidget(buttons);
	connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
	connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

	// Controllers must keep reporting while another window (or the test capture) has focus.
	SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
	m_sdl_ready = SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER) == 0;
	if (m_sdl_ready) {
		m_poll_timer = new QTimer(this);
		connect(m_poll_timer, &QTimer::timeout, this, [this]() { PollControllers(); });
		m_poll_timer->start(POLL_INTERVAL_MS);
	} else {
		m_test_label->setText(tr("Controller support could not start: %1")
		                          .arg(QString::fromUtf8(SDL_GetError())));
	}

	RefreshControllerList();
	RefreshSelectionChoices(controller_selection);
	UpdateButtons();
}

InputMappingDialog::~InputMappingDialog() {
	if (m_poll_timer != nullptr) {
		m_poll_timer->stop();
	}
	for (const auto& pad: std::as_const(m_pads)) {
		SDL_GameControllerClose(static_cast<SDL_GameController*>(pad.handle));
	}
	m_pads.clear();
	if (m_sdl_ready) {
		SDL_QuitSubSystem(SDL_INIT_GAMECONTROLLER);
	}
}

QWidget* InputMappingDialog::CreateControllerTab(const QStringList& controller_mapping) {
	auto* tab    = new QWidget(this);
	auto* layout = new QVBoxLayout(tab);

	auto* intro = new QLabel(tr("Connect a controller and press any button. PlayStation, Xbox, and "
	                            "most other controllers are set up automatically."),
	                         tab);
	intro->setWordWrap(true);
	layout->addWidget(intro);

	auto* pads_box    = new QGroupBox(tr("Connected controllers"), tab);
	auto* pads_layout = new QVBoxLayout(pads_box);
	m_pad_list        = new QListWidget(pads_box);
	m_pad_list->setSelectionMode(QAbstractItemView::NoSelection);
	m_pad_list->setMaximumHeight(96);
	pads_layout->addWidget(m_pad_list);
	layout->addWidget(pads_box);

	auto* use_box    = new QGroupBox(tr("Which controller plays the game"), tab);
	auto* use_layout = new QVBoxLayout(use_box);
	m_selection      = new QComboBox(use_box);
	use_layout->addWidget(m_selection);
	auto* use_note = new QLabel(tr("The other connected controllers are ignored, so a controller "
	                               "someone else is using on this PC cannot take over."),
	                            use_box);
	use_note->setWordWrap(true);
	use_layout->addWidget(use_note);
	layout->addWidget(use_box);

	auto* test_box    = new QGroupBox(tr("Test"), tab);
	auto* test_layout = new QVBoxLayout(test_box);
	m_test_label      = new QLabel(tr("Press a button on any controller."), test_box);
	auto test_font    = m_test_label->font();
	test_font.setBold(true);
	test_font.setPointSizeF(test_font.pointSizeF() * 1.15);
	m_test_label->setFont(test_font);
	m_test_label->setWordWrap(true);
	m_axes_label = new QLabel(tr("Move a stick or pull a trigger to see its position."), test_box);
	m_axes_label->setWordWrap(true);
	test_layout->addWidget(m_test_label);
	test_layout->addWidget(m_axes_label);
	layout->addWidget(test_box);

	auto* map_box    = new QGroupBox(tr("Button layout"), tab);
	auto* map_layout = new QVBoxLayout(map_box);
	m_pad_mode_label = new QLabel(map_box);
	m_pad_mode_label->setWordWrap(true);
	map_layout->addWidget(m_pad_mode_label);
	m_pad_bindings = CreateBindingTree(map_box, tr("Controller button"));
	map_layout->addWidget(m_pad_bindings, 1);

	const auto parsed = ParseMapping(controller_mapping);
	m_pad_custom      = !parsed.isEmpty();
	for (auto* item: BindingItems(m_pad_bindings)) {
		const auto& control = ControlOf(item);
		SetPadBinding(item, m_pad_custom ? parsed.value(QString::fromLatin1(control.id))
		                                 : QString::fromLatin1(control.default_pad_button));
	}
	UpdatePadModeLabel();

	auto* controls = new QHBoxLayout;
	m_pad_change   = new QPushButton(tr("Change..."), map_box);
	m_pad_clear    = new QPushButton(tr("Clear"), map_box);
	auto* defaults = new QPushButton(tr("Reset to automatic"), map_box);
	controls->addWidget(m_pad_change);
	controls->addWidget(m_pad_clear);
	controls->addWidget(defaults);
	controls->addStretch();
	map_layout->addLayout(controls);
	layout->addWidget(map_box, 1);

	connect(m_pad_bindings, &QTreeWidget::itemDoubleClicked, this,
	        [this](QTreeWidgetItem* item, int) {
		        if (IsBindingItem(item)) {
			        ChangePadBinding();
		        }
	        });
	connect(m_pad_bindings, &QTreeWidget::itemSelectionChanged, this, [this]() { UpdateButtons(); });
	connect(m_pad_change, &QPushButton::clicked, this, [this]() { ChangePadBinding(); });
	connect(m_pad_clear, &QPushButton::clicked, this, [this]() { ClearPadBinding(); });
	connect(defaults, &QPushButton::clicked, this, [this]() { RestorePadDefaults(); });
	return tab;
}

QWidget* InputMappingDialog::CreateKeyboardTab(const QStringList& keyboard_mapping) {
	auto* tab    = new QWidget(this);
	auto* layout = new QVBoxLayout(tab);

	auto* intro = new QLabel(tr("Play with the keyboard and mouse. Double-click a row to change its "
	                            "key.\nPress F7 in-game to use mouse movement as the right stick."),
	                         tab);
	intro->setWordWrap(true);
	layout->addWidget(intro);

	const auto parsed = ParseMapping(keyboard_mapping);
	m_key_custom      = !parsed.isEmpty();

	auto* sensitivity_layout = new QHBoxLayout;
	sensitivity_layout->addWidget(new QLabel(tr("Mouse sensitivity"), tab));
	m_sensitivity = new QDoubleSpinBox(tab);
	m_sensitivity->setRange(0.1, 5.0);
	m_sensitivity->setSingleStep(0.1);
	m_sensitivity->setDecimals(1);
	m_sensitivity->setSuffix(QStringLiteral("x"));
	m_sensitivity->setValue(ParseMouseSensitivity(keyboard_mapping));
	sensitivity_layout->addWidget(m_sensitivity);
	sensitivity_layout->addStretch();
	layout->addLayout(sensitivity_layout);

	m_key_bindings = CreateBindingTree(tab, tr("Keyboard / mouse"));
	layout->addWidget(m_key_bindings, 1);
	for (auto* item: BindingItems(m_key_bindings)) {
		const auto& control = ControlOf(item);
		SetKeyBinding(item, m_key_custom ? parsed.value(QString::fromLatin1(control.id))
		                                 : QString::fromLatin1(control.default_key));
	}
	m_key_bindings->setCurrentItem(BindingItems(m_key_bindings).value(0));

	auto* controls = new QHBoxLayout;
	m_key_change   = new QPushButton(tr("Change..."), tab);
	m_key_clear    = new QPushButton(tr("Clear"), tab);
	auto* defaults = new QPushButton(tr("Defaults"), tab);
	controls->addWidget(m_key_change);
	controls->addWidget(m_key_clear);
	controls->addWidget(defaults);
	controls->addStretch();
	layout->addLayout(controls);

	connect(m_key_bindings, &QTreeWidget::itemDoubleClicked, this,
	        [this](QTreeWidgetItem* item, int) {
		        if (IsBindingItem(item)) {
			        ChangeKeyBinding();
		        }
	        });
	connect(m_key_bindings, &QTreeWidget::itemSelectionChanged, this, [this]() { UpdateButtons(); });
	connect(m_key_change, &QPushButton::clicked, this, [this]() { ChangeKeyBinding(); });
	connect(m_key_clear, &QPushButton::clicked, this, [this]() { ClearKeyBinding(); });
	connect(defaults, &QPushButton::clicked, this, [this]() { RestoreKeyDefaults(); });
	return tab;
}

QStringList InputMappingDialog::Mapping() const {
	QStringList result;
	if (m_key_custom) {
		for (const auto* item: BindingItems(m_key_bindings)) {
			const auto binding = item->data(BINDING_COLUMN, Qt::UserRole).toString();
			if (!binding.isEmpty()) {
				result.append(item->data(0, Qt::UserRole).toString() + QLatin1Char('=') + binding);
			}
		}
	}
	if (m_sensitivity->value() != DEFAULT_MOUSE_SENSITIVITY) {
		result.append(QLatin1String(MOUSE_SENSITIVITY) +
		              QString::number(m_sensitivity->value(), 'f', 1));
	}
	return result;
}

QStringList InputMappingDialog::ControllerMapping() const {
	QStringList result;
	if (!m_pad_custom) {
		return result;
	}
	for (const auto* item: BindingItems(m_pad_bindings)) {
		const auto binding = item->data(BINDING_COLUMN, Qt::UserRole).toString();
		if (!binding.isEmpty()) {
			result.append(item->data(0, Qt::UserRole).toString() + QLatin1Char('=') + binding);
		}
	}
	return result;
}

QString InputMappingDialog::ControllerSelection() const {
	const auto value = m_selection != nullptr ? m_selection->currentData().toString() : QString();
	return value.isEmpty() ? QString::fromLatin1(SELECTION_FIRST) : value;
}

void InputMappingDialog::PollControllers() {
	int     axis_pad = -1;
	SDL_Event event;
	while (SDL_PollEvent(&event) != 0) {
		switch (event.type) {
			case SDL_CONTROLLERDEVICEADDED: OpenController(event.cdevice.which); break;
			case SDL_CONTROLLERDEVICEREMOVED: CloseController(event.cdevice.which); break;
			case SDL_CONTROLLERBUTTONDOWN:
				OnControllerButton(event.cbutton.which, event.cbutton.button);
				break;
			case SDL_CONTROLLERAXISMOTION:
				if (std::abs(event.caxis.value) > AXIS_TEST_THRESHOLD) {
					axis_pad = event.caxis.which;
				}
				break;
			default: break;
		}
	}
	if (axis_pad >= 0) {
		OnControllerAxis(axis_pad);
	}
}

void InputMappingDialog::OpenController(int device_index) {
	auto* pad = SDL_GameControllerOpen(device_index);
	if (pad == nullptr) {
		return;
	}
	const int id = SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(pad));
	if (m_pads.contains(id)) {
		SDL_GameControllerClose(pad);
		return;
	}
	const char* name = SDL_GameControllerName(pad);
	m_pads.insert(id, Pad {pad, name != nullptr ? QString::fromUtf8(name) : tr("Unknown controller"),
	                       static_cast<int>(SDL_GameControllerGetType(pad))});
	RefreshControllerList();
	RefreshSelectionChoices(ControllerSelection());
	RefreshPadButtonNames();
}

void InputMappingDialog::CloseController(int instance_id) {
	const auto pad = m_pads.find(instance_id);
	if (pad == m_pads.end()) {
		return;
	}
	SDL_GameControllerClose(static_cast<SDL_GameController*>(pad->handle));
	m_pads.erase(pad);
	if (m_last_pad == instance_id) {
		m_last_pad = -1;
	}
	RefreshControllerList();
	RefreshSelectionChoices(ControllerSelection());
	RefreshPadButtonNames();
}

void InputMappingDialog::RefreshControllerList() {
	m_pad_list->clear();
	if (!m_sdl_ready) {
		m_pad_list->addItem(tr("Controller support is unavailable."));
		return;
	}
	if (m_pads.isEmpty()) {
		m_pad_list->addItem(tr("No controllers detected. Connect one and it appears here."));
		return;
	}
	for (auto pad = m_pads.cbegin(); pad != m_pads.cend(); ++pad) {
		auto text = tr("%1  (%2)").arg(pad->name, ControllerTypeName(pad->type));
		if (pad.key() == m_last_pad) {
			text += tr("   ← last pressed");
		}
		m_pad_list->addItem(text);
	}
}

void InputMappingDialog::RefreshSelectionChoices(const QString& keep) {
	const QString current = keep.isEmpty() ? QString::fromLatin1(SELECTION_FIRST) : keep;

	QStringList names;
	for (const auto& pad: std::as_const(m_pads)) {
		if (!names.contains(pad.name)) {
			names.append(pad.name);
		}
	}
	if (current.startsWith(QLatin1String(SELECTION_NAME_PREFIX))) {
		const auto saved = current.mid(static_cast<int>(sizeof(SELECTION_NAME_PREFIX) - 1));
		if (!saved.isEmpty() && !names.contains(saved)) {
			names.append(saved);
		}
	}

	const QSignalBlocker blocker(m_selection);
	m_selection->clear();
	m_selection->addItem(tr("The first controller you press a button on (recommended)"),
	                     QString::fromLatin1(SELECTION_FIRST));
	m_selection->addItem(tr("Whichever controller was used most recently"),
	                     QString::fromLatin1(SELECTION_LAST));
	for (const auto& name: names) {
		m_selection->addItem(tr("Only controllers named \"%1\"").arg(name),
		                     QString::fromLatin1(SELECTION_NAME_PREFIX) + name);
	}
	const int index = m_selection->findData(current);
	m_selection->setCurrentIndex(index >= 0 ? index : 0);
}

void InputMappingDialog::OnControllerButton(int instance_id, int button) {
	const char* sdl_name =
	    SDL_GameControllerGetStringForButton(static_cast<SDL_GameControllerButton>(button));
	if (sdl_name == nullptr) {
		return;
	}
	const auto sdl_button = QString::fromLatin1(sdl_name);

	if (m_pad_capture != nullptr) {
		m_captured_pad_button = sdl_button;
		m_pad_capture->accept();
		return;
	}

	const auto pad = m_pads.value(instance_id);
	if (m_last_pad != instance_id) {
		m_last_pad = instance_id;
		RefreshControllerList();
		RefreshPadButtonNames();
	}

	QTreeWidgetItem* target = nullptr;
	for (auto* item: BindingItems(m_pad_bindings)) {
		if (item->data(BINDING_COLUMN, Qt::UserRole).toString() == sdl_button) {
			target = item;
			break;
		}
	}
	const auto button_name = PadButtonName(sdl_button, pad.type);
	if (target == nullptr) {
		m_test_label->setText(tr("%1: %2 (not used by the game)").arg(pad.name, button_name));
		return;
	}
	m_pad_bindings->setCurrentItem(target);
	m_pad_bindings->scrollToItem(target);
	m_test_label->setText(tr("%1: %2  →  %3").arg(pad.name, button_name, target->text(0)));
}

void InputMappingDialog::OnControllerAxis(int instance_id) {
	const auto pad = m_pads.constFind(instance_id);
	if (pad == m_pads.cend()) {
		return;
	}
	auto*      handle = static_cast<SDL_GameController*>(pad->handle);
	const auto value  = [handle](SDL_GameControllerAxis axis) {
		 return SDL_GameControllerGetAxis(handle, axis) / 32767.0;
	};
	m_axes_label->setText(tr("%1: left stick %2, %3   right stick %4, %5   L2 %6%   R2 %7%")
	                          .arg(pad->name)
	                          .arg(value(SDL_CONTROLLER_AXIS_LEFTX), 0, 'f', 2)
	                          .arg(value(SDL_CONTROLLER_AXIS_LEFTY), 0, 'f', 2)
	                          .arg(value(SDL_CONTROLLER_AXIS_RIGHTX), 0, 'f', 2)
	                          .arg(value(SDL_CONTROLLER_AXIS_RIGHTY), 0, 'f', 2)
	                          .arg(qRound(value(SDL_CONTROLLER_AXIS_TRIGGERLEFT) * 100.0))
	                          .arg(qRound(value(SDL_CONTROLLER_AXIS_TRIGGERRIGHT) * 100.0)));
}

int InputMappingDialog::CurrentPadType() const {
	if (m_pads.contains(m_last_pad)) {
		return m_pads.value(m_last_pad).type;
	}
	if (!m_pads.isEmpty()) {
		return m_pads.first().type;
	}
	return SDL_CONTROLLER_TYPE_PS4;
}

void InputMappingDialog::RefreshPadButtonNames() {
	for (auto* item: BindingItems(m_pad_bindings)) {
		SetPadBinding(item, item->data(BINDING_COLUMN, Qt::UserRole).toString());
	}
}

void InputMappingDialog::UpdatePadModeLabel() {
	m_pad_mode_label->setText(
	    m_pad_custom ? tr("Custom layout. Controller buttons that are not listed do nothing; the "
	                      "sticks and triggers always work.")
	                 : tr("Automatic layout: every button does what the matching PlayStation "
	                      "button does. Change a row only if you want a different layout."));
}

void InputMappingDialog::SetKeyBinding(QTreeWidgetItem* item, const QString& binding) {
	if (item == nullptr) {
		return;
	}
	item->setData(BINDING_COLUMN, Qt::UserRole, binding);
	item->setText(BINDING_COLUMN, binding.isEmpty() ? tr("None") : binding);
	UpdateButtons();
}

void InputMappingDialog::SetPadBinding(QTreeWidgetItem* item, const QString& sdl_button) {
	if (item == nullptr) {
		return;
	}
	item->setData(BINDING_COLUMN, Qt::UserRole, sdl_button);

	const auto& control = ControlOf(item);
	const auto  analog  = QString::fromLatin1(control.analog_source);
	QString     text;
	if (sdl_button.isEmpty()) {
		text = analog.isEmpty() ? tr("None") : tr("%1 (analog)").arg(analog);
	} else {
		text = PadButtonName(sdl_button, CurrentPadType());
		if (!analog.isEmpty()) {
			text = tr("%1, or %2").arg(text, analog);
		}
	}
	item->setText(BINDING_COLUMN, text);
	UpdateButtons();
}

void InputMappingDialog::ChangeKeyBinding() {
	auto* item = m_key_bindings->currentItem();
	if (!IsBindingItem(item)) {
		return;
	}

	InputCaptureDialog dialog(this);
	if (dialog.exec() != QDialog::Accepted) {
		return;
	}

	for (auto* other: BindingItems(m_key_bindings)) {
		if (other != item && other->data(BINDING_COLUMN, Qt::UserRole)
		                         .toString()
		                         .compare(dialog.Binding(), Qt::CaseInsensitive) == 0) {
			SetKeyBinding(other, {});
		}
	}
	SetKeyBinding(item, dialog.Binding());
	m_key_custom = true;
}

void InputMappingDialog::ClearKeyBinding() {
	auto* item = m_key_bindings->currentItem();
	if (!IsBindingItem(item)) {
		return;
	}
	SetKeyBinding(item, {});
	m_key_custom = true;
}

void InputMappingDialog::RestoreKeyDefaults() {
	for (auto* item: BindingItems(m_key_bindings)) {
		SetKeyBinding(item, QString::fromLatin1(ControlOf(item).default_key));
	}
	m_sensitivity->setValue(DEFAULT_MOUSE_SENSITIVITY);
	m_key_custom = false;
}

void InputMappingDialog::ChangePadBinding() {
	auto* item = m_pad_bindings->currentItem();
	if (!IsBindingItem(item)) {
		return;
	}
	if (!m_sdl_ready || m_pads.isEmpty()) {
		QMessageBox::information(this, tr("Set Controller Button"),
		                         tr("Connect a controller first, then try again."));
		return;
	}

	QDialog capture(this);
	capture.setWindowTitle(tr("Set Controller Button"));
	capture.setModal(true);
	capture.setMinimumWidth(360);
	auto* capture_layout = new QVBoxLayout(&capture);
	auto* label          = new QLabel(
	    tr("Press the controller button for %1.\nEsc cancels.").arg(item->text(0)), &capture);
	label->setAlignment(Qt::AlignCenter);
	capture_layout->addWidget(label);

	// PollControllers keeps running inside exec() and completes the capture.
	m_captured_pad_button.clear();
	m_pad_capture     = &capture;
	const int result = capture.exec();
	m_pad_capture     = nullptr;
	if (result != QDialog::Accepted || m_captured_pad_button.isEmpty()) {
		return;
	}

	for (auto* other: BindingItems(m_pad_bindings)) {
		if (other != item &&
		    other->data(BINDING_COLUMN, Qt::UserRole).toString() == m_captured_pad_button) {
			SetPadBinding(other, {});
		}
	}
	SetPadBinding(item, m_captured_pad_button);
	m_pad_custom = true;
	UpdatePadModeLabel();
}

void InputMappingDialog::ClearPadBinding() {
	auto* item = m_pad_bindings->currentItem();
	if (!IsBindingItem(item)) {
		return;
	}
	SetPadBinding(item, {});
	m_pad_custom = true;
	UpdatePadModeLabel();
}

void InputMappingDialog::RestorePadDefaults() {
	for (auto* item: BindingItems(m_pad_bindings)) {
		SetPadBinding(item, QString::fromLatin1(ControlOf(item).default_pad_button));
	}
	m_pad_custom = false;
	UpdatePadModeLabel();
}

void InputMappingDialog::UpdateButtons() {
	if (m_key_change != nullptr) {
		const auto* item    = m_key_bindings->currentItem();
		const bool  binding = IsBindingItem(item);
		m_key_change->setEnabled(binding);
		m_key_clear->setEnabled(binding &&
		                        !item->data(BINDING_COLUMN, Qt::UserRole).toString().isEmpty());
	}
	if (m_pad_change != nullptr) {
		const auto* item    = m_pad_bindings->currentItem();
		const bool  binding = IsBindingItem(item);
		m_pad_change->setEnabled(binding);
		m_pad_clear->setEnabled(binding &&
		                        !item->data(BINDING_COLUMN, Qt::UserRole).toString().isEmpty());
	}
}
