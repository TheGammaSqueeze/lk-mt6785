require("/scripts/core/core.lua")

CloverBindTarget = class()

function CloverBindTarget.new(arg_1_0)
	local var_1_0 = new(CloverBindTarget)
	local var_1_1, var_1_2 = arg_1_0:getLocalPosition()
	local var_1_3 = arg_1_0:getLocalRotation()
	local var_1_4, var_1_5 = arg_1_0:getLocalScale()

	var_1_0.x = var_1_1
	var_1_0.y = var_1_2
	var_1_0.rot = var_1_3
	var_1_0.sx = var_1_4
	var_1_0.sy = var_1_5
	var_1_0.node = arg_1_0

	return var_1_0
end

function CloverBindTarget.update(arg_2_0, arg_2_1, arg_2_2)
	local var_2_0 = arg_2_0.node
	local var_2_1, var_2_2 = arg_2_1:getLocalPosition()
	local var_2_3 = arg_2_1:getLocalRotation()
	local var_2_4, var_2_5 = arg_2_1:getLocalScale()

	if arg_2_2 then
		var_2_1 = var_2_1 + arg_2_0.x
		var_2_2 = var_2_2 + arg_2_0.y
		var_2_3 = var_2_3 + arg_2_0.rot
		var_2_4 = var_2_4 * arg_2_0.sx
		var_2_5 = var_2_5 * arg_2_0.sy

		var_2_0:setLocalPosition(var_2_1, var_2_2)
		var_2_0:setLocalRotation(var_2_3)
		var_2_0:setLocalScale(var_2_4, var_2_5)
	else
		var_2_0:setLocalPosition(var_2_1, var_2_2)
		var_2_0:setLocalRotation(var_2_3)
		var_2_0:setLocalScale(var_2_4, var_2_5)
	end
end

function CloverBindTarget.reset(arg_3_0)
	local var_3_0 = arg_3_0.node

	var_3_0:setLocalPosition(arg_3_0.x, arg_3_0.y)
	var_3_0:setLocalRotation(arg_3_0.rot)
	var_3_0:setLocalScale(arg_3_0.sx, arg_3_0.sy)
end

CloverNodeAnim = class()

function CloverNodeAnim.new(arg_4_0, arg_4_1, arg_4_2)
	local var_4_0 = new(CloverNodeAnim)

	var_4_0.root = arg_4_0:getChildByName(arg_4_1)

	var_4_0.root:disable()

	var_4_0.root_pane_node = var_4_0.root:getChildByName("RootPane")
	var_4_0.bind_node = var_4_0.root_pane_node:getChildByName(arg_4_2)
	var_4_0.bind_targets = {}
	var_4_0.anim = var_4_0.root_pane_node:getComponent(AnimatorComponent)

	var_4_0.anim:stop()

	var_4_0.is_relative = true

	return var_4_0
end

function CloverNodeAnim.Bind(arg_5_0, arg_5_1)
	local var_5_0 = #arg_5_0.bind_targets

	if var_5_0 == 0 then
		arg_5_0.root:enable()
	end

	arg_5_0.bind_targets[var_5_0 + 1] = CloverBindTarget.new(arg_5_1)
end

function CloverNodeAnim.ClearBind(arg_6_0)
	arg_6_0.bind_targets = {}

	arg_6_0.root:disable()
end

function CloverNodeAnim.Reset(arg_7_0)
	for iter_7_0 = 1, #arg_7_0.bind_targets do
		arg_7_0.bind_targets[iter_7_0]:reset()
	end

	arg_7_0.bind_targets = {}

	arg_7_0.root:disable()
end

function CloverNodeAnim.ChangeAnim(arg_8_0, arg_8_1, arg_8_2)
	arg_8_0.anim:setSceneAnimation(arg_8_0.root[arg_8_1])
	arg_8_0.anim:setCurrentTime(0)
	arg_8_0.anim:setLoop(arg_8_2)
	arg_8_0.anim:play()
end

function CloverNodeAnim.isAnimStopped(arg_9_0)
	return arg_9_0.anim:isStopped()
end

function CloverNodeAnim.getRoot(arg_10_0)
	return arg_10_0.root
end

function CloverNodeAnim.update(arg_11_0, arg_11_1)
	for iter_11_0 = 1, #arg_11_0.bind_targets do
		arg_11_0.bind_targets[iter_11_0]:update(arg_11_0.bind_node, arg_11_0.is_relative)
	end
end
