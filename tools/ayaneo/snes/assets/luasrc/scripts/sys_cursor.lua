require("/scripts/core/core.lua")
require("/scripts/tween/tween_ease.lua")

local var_0_0 = 0.5
local var_0_1 = Ease.outExpo

sys_cursor = class(WorldNode)

function sys_cursor.start(arg_1_0)
	local var_1_0 = {}

	for iter_1_0 in iterate_children(arg_1_0) do
		var_1_0[tonumber(iter_1_0:getName())] = iter_1_0
		iter_1_0.visual = iter_1_0:getComponent(SpriteComponent)
	end

	arg_1_0.nodes = var_1_0
	arg_1_0.parent = arg_1_0:getParentNode()

	local var_1_1, var_1_2 = var_1_0[1].visual:getSize()
	local var_1_3, var_1_4 = var_1_0[1].visual:getPivot()

	arg_1_0.w, arg_1_0.h = var_1_1 * (1 - var_1_3), var_1_2 * (1 - var_1_4)

	-- The cursor colour pulses (1,1,1)->(0.75,0.75,1) over a 2.25s cycle. The DEVICE does this too: a
	-- hi-fps idle capture of the menubar square cursor (properly focused) shows its green channel
	-- oscillating ~207<->181 with a ~2.25s period, matching this loop. (An earlier run wrongly removed
	-- this after measuring an UNFOCUSED menubar - no cursor present - and a box-art-confounded carousel
	-- region; both read constant, so the pulse looked absent. It is not.)
	for iter_1_1, iter_1_2 in pairs(var_1_0) do
		if not iter_1_2.color_tween then
			iter_1_2.color_tween = Tween:loop(math.huge, Tween:sequence(Tween:colorTo(iter_1_2.visual, 0.5, 1, 1, 1, 1), Tween:wait(0.75), Tween:colorTo(iter_1_2.visual, 0.75, 0.75, 0.75, 1, 1), Tween:wait(0.25))):start()
		end
	end
end

function sys_cursor.moveToComponent(arg_2_0, arg_2_1, arg_2_2, arg_2_3)
	arg_2_0.target = arg_2_1

	local var_2_0 = var_0_0

	if arg_2_2 then
		var_2_0 = 0
	end

	local var_2_1 = arg_2_0.nodes
	local var_2_2, var_2_3 = arg_2_1:getSize()
	local var_2_4 = {
		{
			-var_2_2 / 2,
			var_2_3 / 2
		},
		{
			0,
			var_2_3 / 2
		},
		{
			var_2_2 / 2,
			var_2_3 / 2
		},
		{
			-var_2_2 / 2,
			0
		},
		{
			0,
			0
		},
		{
			var_2_2 / 2,
			0
		},
		{
			-var_2_2 / 2,
			-var_2_3 / 2
		},
		{
			0,
			-var_2_3 / 2
		},
		{
			var_2_2 / 2,
			-var_2_3 / 2
		}
	}
	local var_2_5 = {}

	for iter_2_0, iter_2_1 in ipairs(var_2_1) do
		iter_2_1.oldX, iter_2_1.oldY = iter_2_1:getWorldPosition()
	end

	arg_2_0:setNewParent(arg_2_1:getNode())
	arg_2_0:setZIndex(0)

	for iter_2_2, iter_2_3 in ipairs(var_2_1) do
		iter_2_3:setWorldPosition(iter_2_3.oldX, iter_2_3.oldY)

		local var_2_6, var_2_7 = unpack(var_2_4[iter_2_2])

		if arg_2_2 then
			iter_2_3:setLocalPosition(var_2_6, var_2_7)
		else
			local var_2_8 = Tween:moveTo(iter_2_3, var_2_0, var_2_6, var_2_7, var_0_1)

			table.insert(var_2_5, var_2_8)
		end
	end

	local var_2_9 = var_2_2 - arg_2_0.w * 2 + 1
	local var_2_10 = var_2_3 - arg_2_0.h * 2 + 1

	if arg_2_2 then
		local var_2_11, var_2_12 = var_2_1[1].visual:getSize()

		var_2_1[2].visual:setSize(var_2_9, var_2_12)
		var_2_1[8].visual:setSize(var_2_9, var_2_12)
		var_2_1[4].visual:setSize(var_2_11, var_2_10)
		var_2_1[6].visual:setSize(var_2_11, var_2_10)
		var_2_1[5].visual:setSize(var_2_9, var_2_10)
	else
		if arg_2_3 then
			arg_2_0:setZIndex(arg_2_3)
		end

		table.insert(var_2_5, Tween:animate(var_2_1[2].visual, var_2_0, "to", {
			SizeX = var_2_9
		}, var_0_1))
		table.insert(var_2_5, Tween:animate(var_2_1[8].visual, var_2_0, "to", {
			SizeX = var_2_9
		}, var_0_1))
		table.insert(var_2_5, Tween:animate(var_2_1[4].visual, var_2_0, "to", {
			SizeY = var_2_10
		}, var_0_1))
		table.insert(var_2_5, Tween:animate(var_2_1[6].visual, var_2_0, "to", {
			SizeY = var_2_10
		}, var_0_1))
		table.insert(var_2_5, Tween:animate(var_2_1[5].visual, var_2_0, "to", {
			SizeX = var_2_9,
			SizeY = var_2_10
		}, var_0_1))
	end

	tween_stop(arg_2_0.tween)

	if not arg_2_2 then
		arg_2_0.tween = Tween:sequence(Tween:parallel(unpack(var_2_5)), Tween:callback(function()
			arg_2_0:setZIndex(0)
		end)):start()
	end
end
