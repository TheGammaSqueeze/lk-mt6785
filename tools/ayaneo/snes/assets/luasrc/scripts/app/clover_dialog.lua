require("/scripts/core/core.lua")
require("/scripts/app/clover_task.lua")
require("/scripts/app/clover_util.lua")
require("/scripts/app/clover_event.lua")
require("/scripts/app/clover_pad.lua")

local var_0_0 = {}
local var_0_1 = {}

CloverDialogListener = class()

function CloverDialogListener.new(arg_1_0)
	local var_1_0 = new(CloverDialogListener)

	var_1_0.commands = {}

	local function var_1_1()
		return
	end

	var_1_0.open = {}
	var_1_0.open.func_fadeout_end = var_1_1
	var_1_0.open.func_open_end = var_1_1
	var_1_0.open.func_dialog_yes = var_1_1
	var_1_0.open.func_dialog_no = var_1_1
	var_1_0.close = {}
	var_1_0.close.func_close_end = var_1_1
	var_1_0.close.func_fadein_end = var_1_1
	var_1_0.listen_func = arg_1_0

	return var_1_0
end

CloverDialogListener.do_nothing = CloverDialogListener.new(function(arg_3_0)
	return
end)

function CloverDialogListener.listen(arg_4_0, arg_4_1)
	print(string.format("--- dialog event: %s ---", arg_4_1))

	arg_4_0.commands[arg_4_1] = true

	local var_4_0 = {
		fade_out = arg_4_0.open.func_fadeout_end,
		opened = arg_4_0.open.func_open_end,
		yes = arg_4_0.open.func_dialog_yes,
		no = arg_4_0.open.func_dialog_no,
		closeed = arg_4_0.close.func_close_end,
		fade_in = arg_4_0.close.func_fadein_end
	}

	if var_4_0[arg_4_1] then
		var_4_0[arg_4_1]()
	end

	if arg_4_0.listen_func then
		arg_4_0.listen_func(arg_4_1)
	end
end

function CloverDialogListener.set_listen_func(arg_5_0, arg_5_1)
	arg_5_0.listen_func = arg_5_1
end

function CloverDialogListener.clear_commands(arg_6_0)
	arg_6_0.commands = {}
end

function CloverDialogListener.get_commands(arg_7_0)
	return arg_7_0.commands
end

function CloverDialogListener.command_exists(arg_8_0, arg_8_1)
	return arg_8_0.commands[arg_8_1]
end

CloverDialog = {
	Init = function(arg_9_0, arg_9_1, arg_9_2, arg_9_3)
		local var_9_0 = CloverDialog

		var_9_0.fade = arg_9_0
		var_9_0.dialog_ok = arg_9_1
		var_9_0.dialog_yesno = arg_9_2
		var_9_0.dialog = arg_9_2
		var_9_0.main = arg_9_3
		var_9_0.state = CloverState.new()

		var_9_0.state:add("wait", var_0_1.wait)
		var_9_0.state:add("open", var_0_1.open)
		var_9_0.state:add("close", var_0_1.close)
		var_9_0.state:change("wait")

		var_9_0.event = CloverEvent.new()
		var_9_0.open_arg = {}
		var_9_0.close_arg = {}
		var_9_0.listener = CloverDialogListener.do_nothing
		var_9_0.is_busy = false

		CloverTask.Register("dialog", CloverDialog.Update)
	end,
	Update = function(arg_10_0)
		CloverDialog.state:update(arg_10_0)
	end,
	SetText = function(arg_11_0, arg_11_1, arg_11_2)
		if arg_11_2 then
			CloverDialog.dialog = CloverDialog.dialog_yesno
		else
			CloverDialog.dialog = CloverDialog.dialog_ok
		end

		CloverDialog.dialog:setText(arg_11_0)
		CloverDialog.dialog:setButtonText(arg_11_1, arg_11_2)
	end,
	SetTextLabel = function(arg_12_0, arg_12_1, arg_12_2)
		local var_12_0 = Localization.getText(arg_12_0)
		local var_12_1 = Localization.getText(arg_12_1)

		if arg_12_2 then
			local var_12_2 = Localization.getText(arg_12_2)

			CloverDialog.SetText(var_12_0, var_12_1, var_12_2)
		else
			CloverDialog.SetText(var_12_0, var_12_1, nil)
		end
	end,
	SetFocusYes = function()
		CloverDialog.dialog_yesno:setFocusYes()
	end,
	SetFocusNo = function()
		CloverDialog.dialog_yesno:setFocusNo()
	end,
	Open = function(arg_15_0, arg_15_1, arg_15_2, arg_15_3)
		var_0_0.start()

		local var_15_0 = CloverDialog

		var_15_0.open_arg = {}
		var_15_0.open_arg.invoker = arg_15_0
		var_15_0.open_arg.is_fade_out = arg_15_3
		var_15_0.open_arg.is_auto_close = arg_15_2

		if arg_15_1 then
			var_15_0.listener = arg_15_1
		else
			var_15_0.listener = CloverDialogListener.do_nothing
		end

		var_15_0.listener:clear_commands()
		var_15_0.state:set_next("open")
	end,
	OpenFullScreen = function(arg_16_0, arg_16_1, arg_16_2)
		CloverDialog.Open(arg_16_0, arg_16_1, arg_16_2, true)
	end,
	Close = function(arg_17_0, arg_17_1)
		var_0_0.start()

		local var_17_0 = CloverDialog

		var_17_0.close_arg = {}
		var_17_0.close_arg.is_fade_in = arg_17_1

		if arg_17_0 then
			var_17_0.listener = arg_17_0
		else
			var_17_0.listener = CloverDialogListener.do_nothing
		end

		var_17_0.listener:clear_commands()
		var_17_0.state:set_next("close")
	end,
	CloseFullScreen = function(arg_18_0)
		CloverDialog.Close(arg_18_0, is_fade_in)
	end
}

function var_0_0.start()
	assert(CloverDialog.is_busy == false)

	CloverDialog.is_busy = true

	CloverPadUI:EnableUserInput(false, "dialog")
end

function var_0_0.finish()
	CloverDialog.is_busy = false

	CloverPadUI:EnableUserInput(true, "dialog")
end

function var_0_1.wait(arg_21_0)
	return
end

function var_0_1.open(arg_22_0)
	local var_22_0 = CloverDialog
	local var_22_1 = var_22_0.event
	local var_22_2 = var_22_0.listener

	if arg_22_0:is_first() then
		var_22_1:reset()
	end

	local function var_22_3()
		CloverDialog.listener:listen("yes")
	end

	local function var_22_4()
		CloverDialog.listener:listen("no")
	end

	local var_22_5 = {
		function()
			if var_22_0.open_arg.is_fade_out then
				var_22_0.fade:Out(0, nil, nil)
			end

			var_22_1:next_index()
		end,
		function()
			if var_22_0.fade:isRunning() == false then
				if var_22_0.open_arg.is_fade_out then
					var_22_1:wait(0.25)
				end

				var_22_2:listen("fade_out")
				var_22_1:next_index()
			end
		end,
		function()
			if var_22_0.open_arg.is_fade_out then
				var_22_0.main:disable()
			end

			local var_27_0 = var_22_0.open_arg
			local var_27_1 = var_27_0.is_auto_close == false

			var_22_0.dialog:showDialog(var_27_0.invoker, var_22_3, var_22_4, var_27_1)
			var_22_0.dialog_ok:resetFocus()
			var_22_0.dialog_yesno:resetFocus()
			var_22_1:next_index()
		end,
		function()
			if var_22_0.dialog:isAnimation() == false then
				var_22_2:listen("opened")
				var_0_0.finish()
				arg_22_0:set_next("wait")
				var_22_1:next_index()
			end
		end
	}

	var_22_1:update(var_22_5, arg_22_0.dt)
end

function var_0_1.close(arg_29_0)
	local var_29_0 = CloverDialog
	local var_29_1 = var_29_0.event
	local var_29_2 = var_29_0.listener

	if arg_29_0:is_first() then
		var_29_1:reset()
	end

	local var_29_3 = {
		function()
			if not var_29_0.dialog:isClosed() then
				var_29_0.dialog:closeDialog()
			end

			var_29_1:next_index()
		end,
		function()
			if var_29_0.dialog:isAnimation() == false then
				var_29_2:listen("closed")

				if not var_29_0.close_arg.is_fade_in then
					arg_29_0:set_next("wait")
					var_0_0.finish()
				end

				var_29_1:next_index()
			end
		end,
		function()
			if var_29_0.close_arg.is_fade_in then
				var_29_0.main:enable()
				var_29_0.fade:In(0, nil, nil)
			end

			var_29_1:next_index()
		end,
		function()
			if var_29_0.fade:isRunning() == false then
				arg_29_0:set_next("wait")
				var_29_2:listen("fade_in")
				var_0_0.finish()
				var_29_1:next_index()
			end
		end
	}

	var_29_1:update(var_29_3, arg_29_0.dt)
end
