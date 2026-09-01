require("/scripts/core/core.lua")
require("/scripts/tween/tween.lua")

CloverSingleton = {
	Bind = function(arg_1_0, arg_1_1)
		assert(not arg_1_0.instance)

		arg_1_0.instance = arg_1_1
	end,
	Reset = function(arg_2_0)
		arg_2_0.instance = nil
	end,
	Instance = function(arg_3_0)
		return arg_3_0.instance
	end
}
CloverColor = class()

function CloverColor.new(arg_4_0, arg_4_1, arg_4_2, arg_4_3)
	local var_4_0 = new(CloverColor)

	var_4_0:set(arg_4_0, arg_4_1, arg_4_2, arg_4_3)

	return var_4_0
end

function CloverColor.set(arg_5_0, arg_5_1, arg_5_2, arg_5_3, arg_5_4)
	arg_5_0.r = arg_5_1
	arg_5_0.g = arg_5_2
	arg_5_0.b = arg_5_3
	arg_5_0.a = arg_5_4
end

CloverColor.red = CloverColor.new(1, 0, 0, 1)
CloverColor.blue = CloverColor.new(0, 0, 1, 1)
CloverColor.green = CloverColor.new(0, 1, 0, 1)
CloverColor.white = CloverColor.new(1, 1, 1, 1)
CloverColor.black = CloverColor.new(0, 0, 0, 1)
CloverState = class()

function CloverState.new(arg_6_0)
	local var_6_0 = new(CloverState)

	var_6_0.passed_time = 0
	var_6_0.table = {}
	var_6_0.klass = arg_6_0
	var_6_0.current_state = ""
	var_6_0.next_state = ""
	var_6_0.dt = 0
	var_6_0.local_value = {}

	return var_6_0
end

function CloverState.add(arg_7_0, arg_7_1, arg_7_2)
	arg_7_0.table[arg_7_1] = arg_7_2
end

function CloverState.update(arg_8_0, arg_8_1)
	if not arg_8_0.current_state then
		return
	end

	if arg_8_0.current_state ~= arg_8_0.next_state then
		arg_8_0.current_state = arg_8_0.next_state
		arg_8_0.passed_time = 0
		arg_8_0.local_value = {}
	end

	arg_8_0.dt = arg_8_1

	local var_8_0 = arg_8_0.table[arg_8_0.current_state]

	if arg_8_0.klass then
		var_8_0(arg_8_0.klass, arg_8_0)
	else
		var_8_0(arg_8_0)
	end

	arg_8_0.passed_time = arg_8_0.passed_time + arg_8_1
end

function CloverState.set_next(arg_9_0, arg_9_1)
	arg_9_0.next_state = arg_9_1
end

function CloverState.change(arg_10_0, arg_10_1)
	arg_10_0.current_state = arg_10_1
	arg_10_0.next_state = arg_10_1
	arg_10_0.passed_time = 0
	arg_10_0.local_value = {}
end

function CloverState.is_first(arg_11_0)
	if arg_11_0.passed_time == 0 then
		return true
	else
		return false
	end
end

CloverTitleAccesor = class()

function CloverTitleAccesor.new(arg_12_0)
	local var_12_0 = new(CloverTitleAccesor)

	var_12_0.tbl = titles_list[arg_12_0]

	return var_12_0
end

function CloverTitleAccesor.isSupportedSave(arg_13_0)
	return arg_13_0.tbl.save_count ~= 0
end

function CloverTitleAccesor.getCartridgeSramPath(arg_14_0)
	return PERSISTENT_SUSPENSION_POINTS_PATH .. arg_14_0.tbl.game_code .. "/cartridge.sram"
end

function CloverTitleAccesor.getRecoverSramPath(arg_15_0)
	return PERSISTENT_SUSPENSION_POINTS_PATH .. arg_15_0.tbl.game_code .. "/recover.sram"
end

function CloverTitleAccesor.getSuspendPointPath(arg_16_0, arg_16_1)
	return PERSISTENT_SUSPENSION_POINTS_PATH .. arg_16_0.tbl.game_code .. "/suspendpoint" .. arg_16_1
end

function CloverTitleAccesor.getResumeSramPath(arg_17_0, arg_17_1)
	local var_17_0 = arg_17_0.tbl.persistent_saves

	if var_17_0[arg_17_1] == nil then
		return nil
	end

	return var_17_0[arg_17_1].folder_name .. "/resume.sram"
end

function CloverTitleAccesor.getCartridgeSramHash(arg_18_0)
	return arg_18_0:getCartridgeSramPath() .. ".hash"
end

function CloverTitleAccesor.getSuspendPointSramHash(arg_19_0, arg_19_1)
	return arg_19_0:getSuspendPointPath(arg_19_1) .. "/rollback/sram.hash"
end

function MakeBlinkTween(arg_20_0, arg_20_1, arg_20_2)
	local function var_20_0()
		if arg_20_2 then
			arg_20_2:play()
		end
	end

	return Tween:sequence(Tween:worldNodeEnabledTo(arg_20_0, arg_20_1, false), Tween:worldNodeEnabledTo(arg_20_0, arg_20_1, true), Tween:callback(var_20_0), Tween:worldNodeEnabledTo(arg_20_0, arg_20_1, false), Tween:worldNodeEnabledTo(arg_20_0, arg_20_1, true), Tween:callback(var_20_0), Tween:worldNodeEnabledTo(arg_20_0, arg_20_1, false), Tween:worldNodeEnabledTo(arg_20_0, arg_20_1, true), Tween:callback(var_20_0), Tween:worldNodeEnabledTo(arg_20_0, arg_20_1, false), Tween:worldNodeEnabledTo(arg_20_0, arg_20_1, true), Tween:callback(var_20_0)):start()
end
