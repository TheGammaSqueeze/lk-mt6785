require("/scripts/core/core.lua")
require("/scripts/app/clover_pad.lua")
require("/scripts/app/clover_demo.lua")
require("/scripts/app/clover_const.lua")
require("/scripts/app/clover_autocancel.lua")

sys_option_setting = class(gui_container)

local var_0_0 = 1
local var_0_1 = 2
local var_0_2 = 3
local var_0_3 = 4

function sys_option_setting.start(arg_1_0)
	gui_container.start(arg_1_0)

	local var_1_0

	if not store.autoShutdown_initialized then
		store.autoShutdown_initialized = true

		local var_1_1 = system.is_nes()
	elseif HOST_PLATFORM_IS_LINUX then
		local var_1_2

		var_1_2 = AUTO_SHUTDOWN_TIMER ~= 0
	else
		local var_1_3 = debug_store.autoShutdown
	end

	arg_1_0.elementArray[var_0_0]:setCurrent(store.setting.myplayDemo)
	arg_1_0.elementArray[var_0_1]:setCurrent(store.setting.autoplayDemo)
	arg_1_0.elementArray[var_0_2]:setCurrent(store.setting.burn_inPreviention)

	local var_1_4 = true

	arg_1_0:changeSetting(arg_1_0.elementArray[var_0_1], var_1_4)
	arg_1_0:changeSetting(arg_1_0.elementArray[var_0_2], var_1_4)
	arg_1_0:refresh()
end

function sys_option_setting.activate(arg_2_0)
	gui_container.activate(arg_2_0)
	arg_2_0:refresh()
	CloverAutoCancel.Set("sys_option_setting", arg_2_0)
end

function sys_option_setting.onButtonClick(arg_3_0, arg_3_1)
	local var_3_0 = arg_3_0.elementArray
	local var_3_1 = table_find(var_3_0, arg_3_1)

	if var_3_1 then
		if var_3_1 ~= var_0_3 then
			arg_3_1:toggle()
			arg_3_1:activate()
			arg_3_0:changeSetting(arg_3_1)
		elseif arg_3_0.dialog then
			arg_3_0.dialog:showDialog(arg_3_0, function()
				system.initialize()
			end)
		end
	end
end

function sys_option_setting.onElementFocus(arg_5_0, arg_5_1)
	gui_container.onElementFocus(arg_5_0, arg_5_1)

	arg_5_0.currentIndex = table_find(arg_5_0.elementArray, arg_5_1)
end

function sys_option_setting.onElementCommand(arg_6_0, arg_6_1, arg_6_2)
	local var_6_0 = arg_6_0.elementArray
	local var_6_1 = table_find(var_6_0, arg_6_1)

	if var_6_1 and var_6_1 ~= var_0_3 and arg_6_2.id == GUI_COMMAND_KEYPRESS then
		if CloverPadUI.pressed.right and arg_6_1.current == false then
			arg_6_1:setOn()
			arg_6_1:activate()

			if arg_6_1.clickedSound then
				arg_6_1.clickedSound:stop()
				arg_6_1.clickedSound:play()
			end

			arg_6_0:changeSetting(arg_6_1)

			return true
		elseif CloverPadUI.pressed.left and arg_6_1.current == true then
			arg_6_1:setOff()
			arg_6_1:activate()

			if arg_6_1.clickedSound then
				arg_6_1.clickedSound:stop()
				arg_6_1.clickedSound:play()
			end

			arg_6_0:changeSetting(arg_6_1)

			return true
		end
	end

	return gui_container.onElementCommand(arg_6_0, arg_6_1, arg_6_2)
end

function sys_option_setting.changeSetting(arg_7_0, arg_7_1, arg_7_2)
	if not store.setting then
		store.setting = {}
	end

	local var_7_0 = arg_7_0.elementArray
	local var_7_1 = table_find(var_7_0, arg_7_1)

	if var_7_1 then
		if var_7_1 == var_0_0 then
			store.setting.myplayDemo = arg_7_1.current

			if arg_7_1.current == true then
				CloverDemo.ToMyplaydemo()
			end
		elseif var_7_1 == var_0_1 then
			store.setting.autoplayDemo = arg_7_1.current

			if arg_7_1.current == true then
				CloverDemo.ToAutodemo()
			end
		elseif var_7_1 == var_0_2 then
			store.setting.burn_inPreviention = arg_7_1.current
		end

		if not arg_7_2 then
			arg_7_0:refresh()
		end
	end
end

function sys_option_setting.onContainerCanceled(arg_8_0, arg_8_1)
	CloverAutoCancel.Set("sys_option_setting", nil)
	Main:toHomeMenu()

	if arg_8_0.cancelSound then
		arg_8_0.cancelSound:stop()
		arg_8_0.cancelSound:play()
	end
end

function sys_option_setting.refresh(arg_9_0)
	if HOST_PLATFORM_IS_LINUX then
		if arg_9_0.elementArray[var_0_2].current == true then
			mcp.set_dimming_timer(CloverConst.System.DIMMING_SECONDS)
		else
			mcp.set_dimming_timer(0)
		end
	end
end
