require("/scripts/core/core.lua")
require("/scripts/tween/tween_ease.lua")
require("/scripts/position_changer_node.lua")

local var_0_0 = 0.2
local var_0_1 = Ease.linear

position_changer = class(WorldNode)

function position_changer.start(arg_1_0)
	arg_1_0:initialize()
end

function position_changer.initialize(arg_2_0)
	for iter_2_0 in iterate_children(arg_2_0) do
		if iter_2_0.ref_node then
			local var_2_0 = iter_2_0.ref_node
			local var_2_1, var_2_2 = iter_2_0.ref_node:getWorldPosition()
			local var_2_3, var_2_4 = iter_2_0:getWorldPosition()

			iter_2_0.orgX, iter_2_0.orgY = var_2_1, var_2_2
			iter_2_0.actX, iter_2_0.actY = var_2_3, var_2_4
		end
	end

	if not arg_2_0.duration then
		arg_2_0.duration = var_0_0
	end
end

function position_changer.changeAnimation(arg_3_0)
	local var_3_0 = arg_3_0.duration or var_0_0

	tween_stop(arg_3_0.tween)

	local var_3_1 = {}

	for iter_3_0 in iterate_children(arg_3_0) do
		local var_3_2 = iter_3_0.ref_node
		local var_3_3 = var_3_2:getParentNode()

		if var_3_2 then
			local var_3_4, var_3_5 = var_3_3:worldToLocalPosition(iter_3_0.actX, iter_3_0.actY)
			local var_3_6 = Tween:moveTo(var_3_2, var_3_0, var_3_4, var_3_5, var_0_1)

			table.insert(var_3_1, var_3_6)
		end
	end

	arg_3_0.tween = Tween:parallel(unpack(var_3_1)):start()
end

function position_changer.revertAnimation(arg_4_0)
	local var_4_0 = arg_4_0.duration or var_0_0

	tween_stop(arg_4_0.tween)

	local var_4_1 = {}

	for iter_4_0 in iterate_children(arg_4_0) do
		local var_4_2 = iter_4_0.ref_node
		local var_4_3 = var_4_2:getParentNode()

		if var_4_2 then
			local var_4_4, var_4_5 = var_4_3:worldToLocalPosition(iter_4_0.orgX, iter_4_0.orgY)
			local var_4_6 = Tween:moveTo(var_4_2, var_4_0, var_4_4, var_4_5, var_0_1)

			table.insert(var_4_1, var_4_6)
		end
	end

	arg_4_0.tween = Tween:parallel(unpack(var_4_1)):start()
end

function position_changer.change(arg_5_0)
	arg_5_0:initialize()

	for iter_5_0 in iterate_children(arg_5_0) do
		local var_5_0 = iter_5_0.ref_node
		local var_5_1 = var_5_0:getParentNode()

		if var_5_0 then
			local var_5_2, var_5_3 = var_5_1:worldToLocalPosition(iter_5_0.actX, iter_5_0.actY)

			var_5_0:setLocalPosition(var_5_2, var_5_3)
		end
	end
end

function position_changer.revert(arg_6_0)
	for iter_6_0 in iterate_children(arg_6_0) do
		local var_6_0 = iter_6_0.ref_node
		local var_6_1 = var_6_0:getParentNode()

		if var_6_0 then
			local var_6_2, var_6_3 = var_6_1:worldToLocalPosition(iter_6_0.orgX, iter_6_0.orgY)

			var_6_0:setLocalPosition(var_6_2, var_6_3)
		end
	end
end
