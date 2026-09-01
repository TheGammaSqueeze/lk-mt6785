require("/scripts/tween/tween_manager.lua")

function tween_stop(arg_1_0)
	if TweenManager and TweenManager.isTweenActive and arg_1_0 and TweenManager:isTweenActive(arg_1_0) then
		TweenManager:stopTween(arg_1_0)
	end
end

function tween_registerProperty()
	local function var_2_0(arg_3_0)
		local var_3_0, var_3_1 = arg_3_0:getWorldPosition()

		return var_3_0
	end

	local function var_2_1(arg_4_0)
		local var_4_0, var_4_1 = arg_4_0:getWorldPosition()

		return var_4_1
	end

	local function var_2_2(arg_5_0, arg_5_1)
		local var_5_0, var_5_1 = arg_5_0:getWorldPosition()

		arg_5_0:setWorldPosition(math.floor(arg_5_1), math.floor(var_5_1 + 0.5))
	end

	local function var_2_3(arg_6_0, arg_6_1)
		local var_6_0, var_6_1 = arg_6_0:getWorldPosition()

		arg_6_0:setWorldPosition(math.floor(var_6_0), math.floor(arg_6_1 + 0.5))
	end

	TweenManager:registerProperty("WorldX", var_2_0, var_2_2)
	TweenManager:registerProperty("WorldY", var_2_1, var_2_3)

	local function var_2_4(arg_7_0)
		local var_7_0, var_7_1 = arg_7_0:getSize()

		return var_7_0
	end

	local function var_2_5(arg_8_0)
		local var_8_0, var_8_1 = arg_8_0:getSize()

		return var_8_1
	end

	local function var_2_6(arg_9_0, arg_9_1)
		local var_9_0, var_9_1 = arg_9_0:getSize()

		arg_9_0:setSize(arg_9_1, var_9_1)
	end

	local function var_2_7(arg_10_0, arg_10_1)
		local var_10_0, var_10_1 = arg_10_0:getSize()

		arg_10_0:setSize(var_10_0, arg_10_1)
	end

	TweenManager:registerProperty("SizeX", var_2_4, var_2_6)
	TweenManager:registerProperty("SizeY", var_2_5, var_2_7)
end

function Tween_worldTo(arg_11_0, arg_11_1, arg_11_2, arg_11_3, arg_11_4, ...)
	return Tween:animate(arg_11_0, arg_11_1, "to", {
		WorldX = arg_11_2,
		WorldY = arg_11_3
	}, arg_11_4, ...)
end

function Tween_scaleTo(arg_12_0, arg_12_1, arg_12_2, arg_12_3, arg_12_4, ...)
	return Tween:animate(arg_12_0, arg_12_1, "to", {
		SizeX = arg_12_2,
		SizeY = arg_12_3
	}, arg_12_4, ...)
end

local var_0_0 = 0

local function var_0_1()
	GUI.disabled = true
end

local function var_0_2()
	GUI.disabled = false
end

function Tween_noControl(...)
	local var_15_0 = {
		Tween:callback(var_0_1),
		...
	}

	table.insert(var_15_0, Tween:callback(var_0_2))

	return Tween:sequence(unpack(var_15_0))
end

local var_0_3 = 0

local function var_0_4()
	GUI.disabledButton = true
end

local function var_0_5()
	GUI.disabledButton = false
end

function Tween_noButton(...)
	local var_18_0 = {
		Tween:callback(var_0_4),
		...
	}

	table.insert(var_18_0, Tween:callback(var_0_5))

	return Tween:sequence(unpack(var_18_0))
end

function Tween_if(arg_19_0, ...)
	if arg_19_0 then
		return Tween:sequence(...)
	else
		return Tween:sequence()
	end
end
