require("/scripts/core/core.lua")

sys_thumbnail_icon = class(WorldNode)

local var_0_0 = 40
local var_0_1 = 30

function sys_thumbnail_icon.start(arg_1_0)
	arg_1_0.orgX, arg_1_0.orgY = arg_1_0:getLocalPosition()
	arg_1_0.cursorX, arg_1_0.cursorY = arg_1_0.cursor_node:getLocalPosition()

	local var_1_0 = new(GUIHLayout)

	var_1_0:initialize(1, 0)

	system.thumbnailIsloading = true

	local var_1_1 = {}

	for iter_1_0, iter_1_1 in ipairs(GameList.order) do
		local var_1_2 = arg_1_0.thumbnail_prefab:instantiate()

		arg_1_0:addChildNode(var_1_2)
		table.insert(var_1_1, var_1_2)
		var_1_2:setInfo(iter_1_1)

		var_1_2.game_index = iter_1_0

		local var_1_3, var_1_4 = var_1_2.component:getSize()
		local var_1_5, var_1_6 = var_1_2.component:getPivot()

		var_1_0:pushBackNode(var_1_2, var_1_3, var_1_4, var_1_5, var_1_6)
	end

	arg_1_0.elementArray = var_1_1

	local var_1_7 = var_1_0:getWidth()
	local var_1_8 = var_0_0 / 2

	arg_1_0.layout = var_1_0

	arg_1_0:setLocalPositionX(arg_1_0.orgX - var_1_7 / 2 + var_1_8)
	arg_1_0:setLocalPositionY(arg_1_0.orgY)

	if REED_DEBUG then
		DbgMenu:entry("sys_thumbnail_icon", arg_1_0)

		system.thumbcur = arg_1_0.cursor_node
	end
end

function sys_thumbnail_icon.update(arg_2_0, arg_2_1)
	if system.thumbnailIsloading and system.loadingCount_thumbnail == 0 then
		local var_2_0
		local var_2_1

		for iter_2_0, iter_2_1 in ipairs(arg_2_0.elementArray) do
			local var_2_2, var_2_3 = iter_2_1.component:getSize()

			arg_2_0.layout:setNodeSize(iter_2_1, var_2_2, var_2_3)

			if iter_2_0 == 1 then
				var_2_0 = var_2_2 / 2
			end

			if iter_2_0 == arg_2_0.current then
				var_2_1 = iter_2_1
			end
		end

		local var_2_4 = arg_2_0.layout:getWidth()

		arg_2_0:setLocalPositionX(arg_2_0.orgX - var_2_4 / 2 + var_2_0)

		local var_2_5, var_2_6 = var_2_1:getLocalPosition()
		local var_2_7, var_2_8 = var_2_1.component:getSize()

		arg_2_0.cursor_node:setLocalPosition(var_2_5, var_2_8)

		system.thumbnailIsloading = false
	end
end

function sys_thumbnail_icon.getElements(arg_3_0)
	return arg_3_0.elementArray
end

function sys_thumbnail_icon.setCurrentIndex(arg_4_0, arg_4_1, arg_4_2)
	local var_4_0 = 1
	local var_4_1 = arg_4_0.current
	local var_4_2

	for iter_4_0, iter_4_1 in ipairs(arg_4_0.elementArray) do
		local var_4_3 = var_0_0
		local var_4_4 = var_0_1

		if iter_4_0 == arg_4_1 then
			iter_4_1:setLocalScale(var_4_0, var_4_0)
			iter_4_1:setZIndex(1)

			var_4_2 = iter_4_1
			arg_4_0.current = arg_4_1
		elseif iter_4_0 == var_4_1 then
			iter_4_1:setLocalScale(1, 1)
			iter_4_1:setZIndex(0)
		end
	end

	if arg_4_0.cursor_node then
		local var_4_5, var_4_6 = var_4_2:getLocalPosition()
		local var_4_7, var_4_8 = var_4_2.component:getSize()

		arg_4_0.cursor_node:setLocalPosition(var_4_5, var_4_8)
	end
end

function sys_thumbnail_icon.setOrder(arg_5_0, arg_5_1)
	local var_5_0 = 1

	arg_5_0.layout:suspend()

	for iter_5_0, iter_5_1 in ipairs(arg_5_0.elementArray) do
		local var_5_1 = arg_5_1[iter_5_0]
		local var_5_2 = iter_5_1

		var_5_2:setInfo(var_5_1)

		var_5_2.game_index = iter_5_0

		local var_5_3, var_5_4 = var_5_2.component:getSize()

		arg_5_0.layout:setNodeSize(var_5_2, var_5_3, var_5_4)

		if iter_5_0 == index then
			iter_5_1:setLocalScale(var_5_0, var_5_0)
			iter_5_1:setZIndex(1)
		else
			iter_5_1:setLocalScale(1, 1)
			iter_5_1:setZIndex(0)
		end
	end

	arg_5_0.layout:resume(true)
end

function sys_thumbnail_icon.setUnselected(arg_6_0, arg_6_1)
	for iter_6_0, iter_6_1 in ipairs(arg_6_0.elementArray) do
		if arg_6_1 ~= nil then
			if iter_6_0 == arg_6_1 then
				iter_6_1.lock:enable()
			else
				iter_6_1.lock:disable()
			end
		else
			iter_6_1.lock:disable()
		end
	end
end

if REED_DEBUG then
	function sys_thumbnail_icon.debug_setLarge(arg_7_0)
		for iter_7_0, iter_7_1 in ipairs(arg_7_0.elementArray) do
			iter_7_1.component:setTexture(iter_7_1.info.spriteResource)
			iter_7_1.component:setSize(var_0_0, var_0_1)
		end
	end

	function sys_thumbnail_icon.debug_setSmall(arg_8_0)
		for iter_8_0, iter_8_1 in ipairs(arg_8_0.elementArray) do
			iter_8_1.component:setTexture(iter_8_1.info.iconResource)
			iter_8_1.component:setSize(var_0_0, var_0_1)
		end
	end
end
