require("../core/core.lua")

GUIGridLayout = class()

function GUIGridLayout.insertElement(arg_1_0, arg_1_1, arg_1_2, arg_1_3)
	local var_1_0 = {
		node = _node,
		index = _index,
		width = _width,
		height = _height,
		centerX = _centerX,
		centerY = _centerY
	}

	arg_1_0.nodes[_node] = var_1_0

	table.insert(arg_1_0.nodesIndexes, _index, var_1_0)

	for iter_1_0 = _index + 1, #arg_1_0.nodesIndexes do
		arg_1_0.nodesIndexes[iter_1_0].index = arg_1_0.nodesIndexes[iter_1_0].index + 1
	end

	if REED_DEBUG then
		arg_1_0:checkConsistency()
	end
end

function GUIGridLayout.refresh(arg_2_0)
	local var_2_0 = 0
	local var_2_1 = -math.huge
	local var_2_2 = 0
	local var_2_3 = -math.huge
	local var_2_4 = arg_2_0.nodesIndexes
	local var_2_5 = #var_2_4
	local var_2_6 = 0
	local var_2_7 = 0
	local var_2_8 = arg_2_0.columnsCount

	arg_2_0.rowInfos = {}
	arg_2_0.columnInfos = {}

	local var_2_9 = 1
	local var_2_10 = 1

	for iter_2_0 = 1, var_2_5 do
		local var_2_11 = var_2_4[iter_2_0]
		local var_2_12 = arg_2_0.rowInfos[var_2_9]
		local var_2_13 = arg_2_0.columnInfos[var_2_10]

		if not var_2_13 then
			var_2_13 = {
				width = 0,
				posX = 0
			}
			arg_2_0.columnInfos[var_2_10] = var_2_13
		end

		if not var_2_12 then
			var_2_12 = {
				posY = 0,
				height = 0
			}
			arg_2_0.rowInfos[var_2_9] = var_2_12
		end

		local var_2_14 = var_2_11.width * var_2_11.centerX + var_2_6
		local var_2_15 = var_2_11.height * var_2_11.centerY + var_2_7

		if var_2_14 > var_2_13.posX then
			var_2_13.posX = var_2_14
		end

		if var_2_15 > var_2_12.posY then
			var_2_12.posY = var_2_15
		end

		local var_2_16 = var_2_13.posX + var_2_11.width * (1 - var_2_11.centerX) - var_2_6
		local var_2_17 = var_2_12.posY + var_2_11.height * (1 - var_2_11.centerY) - var_2_7

		if var_2_16 > var_2_13.width then
			var_2_13.width = var_2_16
		end

		if var_2_17 > var_2_12.height then
			var_2_12.height = var_2_17
		end

		var_2_10 = var_2_10 + 1
		var_2_6 = var_2_6 + var_2_13.width

		if var_2_8 < var_2_10 then
			var_2_1 = var_2_6
			var_2_10 = 1
			var_2_6 = 0
			var_2_9 = var_2_9 + 1
			var_2_7 = var_2_7 + var_2_12.height
		end
	end

	local var_2_18 = var_2_7
	local var_2_19 = 1
	local var_2_20 = 1

	for iter_2_1 = 1, var_2_5 do
		local var_2_21 = var_2_4[iter_2_1]
		local var_2_22 = arg_2_0.columnInfos[var_2_20].posX
		local var_2_23 = arg_2_0.rowInfos[var_2_19].posY

		var_2_21.posX = var_2_22
		var_2_21.posY = var_2_23

		var_2_21.node:setLocalPosition(var_2_22, var_2_23)

		var_2_20 = var_2_20 + 1

		if var_2_8 < var_2_20 then
			var_2_20 = 1
			var_2_19 = var_2_19 + 1
		end
	end

	arg_2_0.xMin = var_2_0
	arg_2_0.xMax = var_2_1
	arg_2_0.yMin = var_2_2
	arg_2_0.yMax = var_2_18
end
