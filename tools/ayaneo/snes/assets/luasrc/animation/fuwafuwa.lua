require("/scripts/core/core.lua")
require("/animation/tweenanimator.lua")

fuwafuwa = class(tweenAnimator)

function fuwafuwa.start(arg_1_0)
	arg_1_0.wing0 = {}
	arg_1_0.wing1 = {}
	arg_1_0.wing2 = {}
	arg_1_0.wing3 = {}
	arg_1_0.fuwatype = "homemenu"

	arg_1_0:setWing(0)

	arg_1_0.looped = false
end

function fuwafuwa.addWing(arg_2_0, arg_2_1)
	if arg_2_1 then
		table.insert(arg_2_0.wing0, arg_2_1.stop)
		table.insert(arg_2_0.wing1, arg_2_1.flap1)
		table.insert(arg_2_0.wing2, arg_2_1.flap)

		if arg_2_1.flap2 then
			table.insert(arg_2_0.wing3, arg_2_1.flap2)
		end
	end
end

function fuwafuwa.getTween(arg_3_0, arg_3_1)
	local var_3_0 = 1
	local var_3_1 = 0.8
	local var_3_2 = 9
	local var_3_3 = 1
	local var_3_4 = 9

	local function var_3_5()
		arg_3_0:setWing(0)
	end

	local function var_3_6()
		arg_3_0:setWing(1)
	end

	local function var_3_7()
		arg_3_0:setWing(2)
	end

	local function var_3_8()
		arg_3_0:setWing(3)
	end

	if arg_3_0.fuwatype == "homemenu" or arg_3_0.fuwatype == "lockmode" then
		var_3_6()

		return Tween:moveTo(arg_3_1, 0.1, 0, 0, Ease.inOutSine):loop(math.huge, Tween:sequence(Tween:moveTo(arg_3_1, var_3_0, 0, var_3_2, Ease.inOutSine), Tween:callback(var_3_5), Tween:parallel(Tween:moveTo(arg_3_1, var_3_0, 0, -var_3_2, Ease.inOutSine), Tween:wait(var_3_0 / 2):callback(var_3_6))))
	elseif arg_3_0.fuwatype == "lockback" then
		var_3_8()

		local var_3_9 = -15
		local var_3_10 = 25
		local var_3_11 = 5

		return Tween:moveTo(arg_3_1, 0.1, 0, var_3_9, Ease.inOutSine):loop(math.huge, Tween:sequence(Tween:moveTo(arg_3_1, 0.5, 0, var_3_9 + var_3_10, Ease.inOutSine), Tween:wait(0.175), Tween:moveTo(arg_3_1, 0.8, 0, var_3_9 - var_3_10, Ease.inOutSine), Tween:moveTo(arg_3_1, 0.5, 0, var_3_9 + var_3_11, Ease.inOutSine), Tween:moveTo(arg_3_1, 0.6, 0, var_3_9 - var_3_11, Ease.inOutSine), Tween:wait(0.125), Tween:callback(var_3_8)))
	elseif arg_3_0.fuwatype == "stop" then
		var_3_5()

		return Tween:moveTo(arg_3_1, 0.1, 0, 0, Ease.inOutSine)
	end
end

function fuwafuwa.setWing(arg_8_0, arg_8_1)
	for iter_8_0, iter_8_1 in ipairs(arg_8_0.wing0) do
		if arg_8_1 == 0 then
			iter_8_1:enable()
		else
			iter_8_1:disable()
		end
	end

	for iter_8_2, iter_8_3 in ipairs(arg_8_0.wing1) do
		if arg_8_1 == 1 then
			iter_8_3:enable()
			iter_8_3:restart()
		else
			iter_8_3:disable()
		end
	end

	for iter_8_4, iter_8_5 in ipairs(arg_8_0.wing2) do
		if arg_8_1 == 2 then
			iter_8_5:enable()
			iter_8_5:restart()
		else
			iter_8_5:disable()
		end
	end

	for iter_8_6, iter_8_7 in ipairs(arg_8_0.wing3) do
		if arg_8_1 == 3 then
			iter_8_7:enable()
			iter_8_7:restart()
		else
			iter_8_7:disable()
		end
	end
end

function fuwafuwa.setType(arg_9_0, arg_9_1)
	arg_9_0.fuwatype = arg_9_1

	arg_9_0:run(ScriptComponent.getNode(arg_9_0))
end
