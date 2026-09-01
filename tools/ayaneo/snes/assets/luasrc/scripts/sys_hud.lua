require("/scripts/core/core.lua")
require("/scripts/system.lua")

sys_hud = class(WorldNode)

function sys_hud.start(arg_1_0)
	if not arg_1_0.elementList then
		arg_1_0.elementList = {}

		for iter_1_0 in iterate_children(arg_1_0) do
			table.insert(arg_1_0.elementList, iter_1_0)
		end

		table.sort(arg_1_0.elementList, function(arg_2_0, arg_2_1)
			return arg_2_0:getName() < arg_2_1:getName()
		end)
	end

	arg_1_0.orgX, arg_1_0.orgY = arg_1_0:getLocalPosition()

	arg_1_0:refresh()
	table.insert(system.hud, arg_1_0)
end

function sys_hud.refresh(arg_3_0)
	local var_3_0 = arg_3_0.VGap or 0
	local var_3_1 = arg_3_0.elementList or {}
	local var_3_2 = 0
	local var_3_3 = {}
	local var_3_4 = 0

	for iter_3_0, iter_3_1 in ipairs(var_3_1) do
		local var_3_5
		local var_3_6

		if iter_3_1.refresh then
			iter_3_1:refresh()
		end

		if iter_3_1.getSize then
			var_3_5, var_3_6 = iter_3_1:getSize()
		elseif iter_3_1:getComponent(VisualComponent) then
			var_3_5, var_3_6 = iter_3_1:getComponent(VisualComponent):getSize()
		end

		var_3_3[iter_3_1] = var_3_4 + var_3_5 / 2
		var_3_4 = var_3_4 + var_3_5 + var_3_0
		var_3_2 = math.max(var_3_2, var_3_6)
	end

	local var_3_7 = var_3_4 - var_3_0
	local var_3_8 = var_3_7 / 2

	if arg_3_0.anchor == "left" then
		var_3_8 = 0
	elseif arg_3_0.anchor == "center" then
		var_3_8 = var_3_7 / 2
	elseif arg_3_0.anchor == "right" then
		var_3_8 = var_3_7
	end

	for iter_3_2, iter_3_3 in ipairs(var_3_1) do
		local var_3_9 = var_3_3[iter_3_3] - var_3_8

		iter_3_3:setLocalPositionX(math.floor(var_3_9))
	end

	arg_3_0.sizeX = var_3_7
	arg_3_0.sizeY = var_3_2
end

function sys_hud.getSize(arg_4_0)
	return arg_4_0.sizeX, arg_4_0.sizeY
end
