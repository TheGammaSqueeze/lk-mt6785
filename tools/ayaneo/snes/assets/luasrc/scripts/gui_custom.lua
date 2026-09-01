require("/scripts/gui/gui.lua")
require("/scripts/gui/gui_element.lua")

gui_custom = {}

local var_0_0 = 3

function gui_custom.initialize(arg_1_0)
	arg_1_0.guiStatusLevel = 0
	arg_1_0.guiStatusName = "default"
	arg_1_0.guiStatus = {}
	arg_1_0.guiConst = {}
	arg_1_0.guiConst = {
		default = {
			V = {
				REPEAT_LOOPED_STOP_DELAY = 0,
				REPEAT_DELAY = 0.5,
				REPEAT_RATE = 0.2,
				REPEAT_LOOPED_RESTART_DELAY = 0.2
			},
			H = {
				REPEAT_LOOPED_STOP_DELAY = 0,
				REPEAT_DELAY = 0.5,
				REPEAT_RATE = 0.2,
				REPEAT_LOOPED_RESTART_DELAY = 0.2
			}
		},
		GameTitleList = {
			REPEAT_ORIENT = true,
			V = {
				REPEAT_LOOPED_STOP_DELAY = 0,
				PRESSED_DELAY = 0,
				REPEAT_OFF = true,
				REPEAT_LOOPED_RESTART_DELAY = 0.2,
				REPEAT_DELAY = 0.5,
				REPEAT_RATE = 0.2
			},
			H = {
				REPEAT_LOOPED_STOP_DELAY = 0.05,
				REPEAT_DELAY = 0.24,
				REPEAT_RATE = 0.09,
				REPEAT_LOOPED_RESTART_DELAY = 0.15
			}
		},
		MenubarUpper = {
			REPEAT_ORIENT = true,
			V = {
				REPEAT_LOOPED_STOP_DELAY = 0,
				PRESSED_DELAY = 0,
				REPEAT_OFF = true,
				REPEAT_LOOPED_RESTART_DELAY = 0.2,
				REPEAT_DELAY = 0.5,
				REPEAT_RATE = 0.2
			},
			H = {
				REPEAT_LOOPED_STOP_DELAY = 0,
				REPEAT_DELAY = 0.5,
				REPEAT_RATE = 0.2,
				REPEAT_LOOPED_RESTART_DELAY = 0.2
			}
		},
		Copyright = {
			REPEAT_ORIENT = true,
			V = {
				REPEAT_LOOPED_STOP_DELAY = 0.05,
				REPEAT_DELAY = 0.3,
				REPEAT_RATE = 0.1,
				REPEAT_LOOPED_RESTART_DELAY = 0.15
			},
			H = {
				REPEAT_LOOPED_STOP_DELAY = 0,
				REPEAT_DELAY = 0.5,
				REPEAT_RATE = 0.2,
				REPEAT_LOOPED_RESTART_DELAY = 0.2
			}
		},
		Resume = {
			REPEAT_ORIENT = true,
			V = {
				REPEAT_LOOPED_STOP_DELAY = 0,
				PRESSED_DELAY = 0,
				REPEAT_OFF = true,
				REPEAT_LOOPED_RESTART_DELAY = 0.2,
				REPEAT_DELAY = 0.5,
				REPEAT_RATE = 0.2
			},
			H = {
				REPEAT_LOOPED_STOP_DELAY = 0,
				REPEAT_DELAY = 0.5,
				REPEAT_RATE = 0.2,
				REPEAT_LOOPED_RESTART_DELAY = 0.2
			}
		},
		DbgMenu = {
			V = {
				REPEAT_LOOPED_STOP_DELAY = 0.3,
				REPEAT_DELAY = 0.3,
				REPEAT_RATE = 0.05,
				REPEAT_LOOPED_RESTART_DELAY = 0.3
			},
			H = {
				REPEAT_LOOPED_STOP_DELAY = 0.3,
				REPEAT_DELAY = 0.3,
				REPEAT_RATE = 0.05,
				REPEAT_LOOPED_RESTART_DELAY = 0.3
			}
		}
	}

	arg_1_0:pushGUIStatus("GameTitleList")
	DbgMenu:entry("gui_custom", arg_1_0)
end

function gui_custom.pushGUIStatus(arg_2_0, arg_2_1)
	arg_2_0.guiStatusLevel = arg_2_0.guiStatusLevel + 1

	if arg_2_1 == nil then
		arg_2_1 = "default"
	end

	print("gui_custom:pushGUIStatus level=" .. tostring(arg_2_0.guiStatusLevel) .. "next=" .. arg_2_1)

	local var_2_0 = {
		name = arg_2_0.guiStatusName,
		focusedElementSave = GUI.focusedElement,
		const = arg_2_0.guiConst[arg_2_0.guiStatusName]
	}

	arg_2_0.guiStatus[arg_2_0.guiStatusLevel] = var_2_0

	local var_2_1 = arg_2_0.guiConst[arg_2_1]

	GUI.H = var_2_1.H
	GUI.V = var_2_1.V
	GUI.REPEAT_ORIENT = var_2_1.REPEAT_ORIENT
	GUI.REPEAT_DELAY = GUI.H.REPEAT_DELAY
	GUI.REPEAT_RATE = GUI.H.REPEAT_RATE
	GUI.state = 0
	arg_2_0.guiStatusName = arg_2_1
end

function gui_custom.popGUIStatus(arg_3_0)
	if arg_3_0.guiStatusLevel <= 0 then
		print("gui_custom:pushGUIStatus: ERROR!!!")
	end

	local var_3_0 = arg_3_0.guiStatus[arg_3_0.guiStatusLevel]

	print("gui_custom:popGUIStatus level=" .. tostring(arg_3_0.guiStatusLevel) .. "next=" .. var_3_0.name)

	local var_3_1 = var_3_0.const

	GUI.H = var_3_1.H
	GUI.V = var_3_1.V
	GUI.REPEAT_ORIENT = var_3_1.REPEAT_ORIENT
	GUI.state = 0
	GUI.REPEAT_DELAY = GUI.H.REPEAT_DELAY
	GUI.REPEAT_RATE = GUI.H.REPEAT_RATE
	arg_3_0.guiStatusName = var_3_0.name
	arg_3_0.guiStatus[arg_3_0.guiStatusLevel] = nil
	arg_3_0.guiStatusLevel = arg_3_0.guiStatusLevel - 1
end
