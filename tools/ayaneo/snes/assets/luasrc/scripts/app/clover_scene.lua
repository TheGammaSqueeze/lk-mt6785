require("/scripts/core/core.lua")
require("/scripts/app/clover_task.lua")

CloverSceneResource = class()

function CloverSceneResource.new(arg_1_0, arg_1_1)
	local var_1_0 = new(CloverSceneResource)

	var_1_0.resource = arg_1_0
	var_1_0.is_linked = false
	var_1_0.link_request = false
	var_1_0.is_add_node = false
	var_1_0.add_node_request = false
	var_1_0.node = nil
	var_1_0.parent_node = arg_1_1
	var_1_0.visible = true

	return var_1_0
end

function CloverSceneResource.Update(arg_2_0)
	if arg_2_0.link_request then
		if arg_2_0.is_linked == false then
			arg_2_0.resource:link()

			arg_2_0.is_linked = true
		end
	elseif arg_2_0.is_linked then
		if arg_2_0.is_add_node then
			arg_2_0.add_node_request = false
		else
			arg_2_0.resource:unlink()

			arg_2_0.is_linked = false
		end
	end

	if arg_2_0.add_node_request then
		if arg_2_0.is_add_node == false and arg_2_0.is_linked and arg_2_0.resource:isLoaded() then
			arg_2_0.node = arg_2_0.resource:instantiate()

			arg_2_0.parent_node:addChildNode(arg_2_0.node)
			arg_2_0:SetVisible(arg_2_0.visible)

			arg_2_0.is_add_node = true
		end
	elseif arg_2_0.is_add_node then
		arg_2_0.node:destroy()

		arg_2_0.node = nil
		arg_2_0.is_add_node = false
	end
end

function CloverSceneResource.GetResource(arg_3_0)
	return arg_3_0.resource
end

function CloverSceneResource.GetNode(arg_4_0)
	return arg_4_0.node
end

function CloverSceneResource.Link(arg_5_0)
	arg_5_0.link_request = true
end

function CloverSceneResource.Unlink(arg_6_0)
	arg_6_0.link_request = false
end

function CloverSceneResource.AddNode(arg_7_0)
	arg_7_0.add_node_request = true
end

function CloverSceneResource.RemoveNode(arg_8_0)
	arg_8_0.add_node_request = false
end

function CloverSceneResource.IsReady(arg_9_0)
	return arg_9_0.is_linked and arg_9_0.is_add_node
end

function CloverSceneResource.SetVisible(arg_10_0, arg_10_1)
	arg_10_0.visible = arg_10_1

	if arg_10_0.node then
		arg_10_0.node:setVisible(arg_10_1)
	end
end

function CloverSceneResource.IsVisible(arg_11_0)
	return arg_11_0.visible
end

function CloverSceneResource.SetParentNode(arg_12_0, arg_12_1)
	arg_12_0.parent_node = arg_12_1

	if arg_12_0.is_add_node then
		arg_12_0.node:setNewParent(arg_12_1)
	end
end

CloverScene = {
	resources = {},
	Init = function(arg_13_0)
		CloverTask.Register("scene", CloverScene.Update)

		CloverScene.root_node = arg_13_0
	end,
	Update = function(arg_14_0)
		for iter_14_0, iter_14_1 in pairs(CloverScene.resources) do
			iter_14_1:Update()
		end
	end,
	AddResource = function(arg_15_0, arg_15_1)
		local var_15_0 = CloverSceneResource.new(arg_15_1, CloverScene.root_node)

		CloverScene.resources[arg_15_0] = var_15_0

		return var_15_0
	end,
	Link = function(arg_16_0)
		CloverScene.resources[arg_16_0]:Link()
	end,
	Unlink = function(arg_17_0)
		CloverScene.resources[arg_17_0]:Unlink()
	end,
	AddNode = function(arg_18_0)
		CloverScene.resources[arg_18_0]:AddNode()
	end,
	RemoveNode = function(arg_19_0)
		CloverScene.resources[arg_19_0]:RemoveNode()
	end,
	IsReady = function(arg_20_0)
		return CloverScene.resources[arg_20_0]:IsReady()
	end,
	SetVisible = function(arg_21_0, arg_21_1)
		CloverScene.resources[arg_21_0]:SetVisible(arg_21_1)
	end,
	IsVisible = function(arg_22_0)
		return CloverScene.resources[arg_22_0]:IsVisible()
	end,
	GetResource = function(arg_23_0)
		return CloverScene.resources[arg_23_0]:GetResource()
	end,
	GetNode = function(arg_24_0)
		return CloverScene.resources[arg_24_0]:GetNode()
	end
}
