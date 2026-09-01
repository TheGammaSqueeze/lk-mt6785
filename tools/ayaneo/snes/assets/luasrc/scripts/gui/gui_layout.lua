require("../core/core.lua")

GUILayout = class()

function GUILayout.initialize(arg_1_0, arg_1_1, arg_1_2)
	arg_1_0.nodes = {}
	arg_1_0.nodesIndexes = {}
	arg_1_0.columnsInfos = {}
	arg_1_0.rowInfos = {}
	arg_1_0.xMin = math.huge
	arg_1_0.xMax = -math.huge
	arg_1_0.yMin = math.huge
	arg_1_0.yMax = -math.huge

	if arg_1_1 then
		arg_1_0.horizontalGap = arg_1_1
	else
		arg_1_0.horizontalGap = 0
	end

	if arg_1_2 then
		arg_1_0.verticalGap = arg_1_2
	else
		arg_1_0.verticalGap = 0
	end
end

function GUILayout.getWidth(arg_2_0)
	return arg_2_0.xMax - arg_2_0.xMin
end

function GUILayout.getHeight(arg_3_0)
	return arg_3_0.yMax - arg_3_0.yMin
end

function GUILayout.suspend(arg_4_0)
	arg_4_0.refreshSuspended = true
end

function GUILayout.resume(arg_5_0, arg_5_1)
	arg_5_0.refreshSuspended = false

	if arg_5_1 == true then
		arg_5_0:refresh()
	end
end

function GUILayout.setNodeSize(arg_6_0, arg_6_1, arg_6_2, arg_6_3)
	local var_6_0 = arg_6_0.nodes[arg_6_1]

	if not var_6_0 then
		GUI:error("[" .. tostring(arg_6_0) .. "]\tGUILayout:setNodeSize() : node does not exist in the nodes list")

		return
	end

	var_6_0.width = arg_6_2
	var_6_0.height = arg_6_3

	if not arg_6_0.refreshSuspended then
		arg_6_0:refresh()
	end
end

function GUILayout.getItemCount(arg_7_0)
	return #arg_7_0.nodesIndexes
end

function GUILayout.getItem(arg_8_0, arg_8_1)
	local var_8_0 = arg_8_0.nodesIndexes[arg_8_1]

	return var_8_0 and var_8_0.node or nil
end

function GUILayout.pushBackNode(arg_9_0, arg_9_1, arg_9_2, arg_9_3, arg_9_4, arg_9_5)
	local var_9_0 = #arg_9_0.nodesIndexes + 1
	local var_9_1 = {
		node = arg_9_1,
		index = var_9_0,
		width = arg_9_2,
		height = arg_9_3,
		centerX = arg_9_4,
		centerY = arg_9_5
	}

	arg_9_0.nodes[arg_9_1] = var_9_1

	table.insert(arg_9_0.nodesIndexes, var_9_1)

	if REED_DEBUG then
		arg_9_0:checkConsistency()
	end

	if not arg_9_0.refreshSuspended then
		arg_9_0:refresh()
	end
end

function GUILayout.pushBackElement(arg_10_0, arg_10_1)
	local var_10_0, var_10_1 = arg_10_1:getSize()
	local var_10_2, var_10_3 = arg_10_1:getPivot()

	arg_10_0:pushBackNode(arg_10_1, var_10_0, var_10_1, var_10_2, var_10_3)
end

function GUILayout.insertNode(arg_11_0, arg_11_1, arg_11_2, arg_11_3, arg_11_4, arg_11_5, arg_11_6)
	local var_11_0 = {
		node = arg_11_2,
		index = arg_11_1,
		width = arg_11_3,
		height = arg_11_4,
		centerX = arg_11_5,
		centerY = arg_11_6
	}

	arg_11_0.nodes[arg_11_2] = var_11_0

	table.insert(arg_11_0.nodesIndexes, _index, var_11_0)

	for iter_11_0 = _index + 1, #arg_11_0.nodesIndexes do
		arg_11_0.nodesIndexes[iter_11_0].index = arg_11_0.nodesIndexes[iter_11_0].index + 1
	end

	if REED_DEBUG then
		arg_11_0:checkConsistency()
	end

	if not arg_11_0.refreshSuspended then
		arg_11_0:refresh()
	end
end

function GUILayout.insertElement(arg_12_0, arg_12_1, arg_12_2)
	local var_12_0, var_12_1 = arg_12_2:getSize()
	local var_12_2, var_12_3 = arg_12_2:getPivot()

	arg_12_0:insertNode(arg_12_1, arg_12_2, var_12_0, var_12_1, var_12_2, var_12_3)
end

function GUILayout.removeByIndex(arg_13_0, arg_13_1)
	local var_13_0 = arg_13_0.nodesIndexes

	if not var_13_0[arg_13_1] then
		GUI:error("[" .. tostring(arg_13_0) .. "]\tGUILayout:removeByIndex() : the provided element index is out of bounds")

		return
	end

	local var_13_1 = var_13_0[arg_13_1].node

	arg_13_0.nodes[var_13_1] = nil

	table.remove(var_13_0, arg_13_1)

	for iter_13_0 = arg_13_1, #var_13_0 do
		var_13_0[iter_13_0].index = var_13_0[iter_13_0].index - 1
	end

	if REED_DEBUG then
		arg_13_0:checkConsistency()
	end

	if not arg_13_0.refreshSuspended then
		arg_13_0:refresh()
	end
end

function GUILayout.removeElement(arg_14_0, arg_14_1)
	local var_14_0 = arg_14_0.nodes[node]

	if not var_14_0 then
		GUI:error("[" .. tostring(arg_14_0) .. "]\tGUILayout:removeElement() : element does not exist in the elements list")

		return
	end

	arg_14_0:removeByIndex(var_14_0.index)
end

function GUILayout.clear(arg_15_0)
	arg_15_0.nodes = {}
	arg_15_0.nodesIndexes = {}
end

if REED_DEBUG then
	function GUILayout.checkConsistency(arg_16_0)
		for iter_16_0 = 1, #arg_16_0.nodesIndexes do
			if arg_16_0.nodesIndexes[iter_16_0].index ~= iter_16_0 then
				GUI:error("[" .. tostring(arg_16_0) .. "]\tGUIScroll:checkConsistency() failed at index " .. tostring(iter_16_0) .. " ;currentIndex = " .. arg_16_0.nodesIndexes[iter_16_0].index)

				return
			end
		end
	end
end
