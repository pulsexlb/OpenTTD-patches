/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file orderlist_gui.cpp The GUI for managing player-created order lists. */

#include "stdafx.h"
#include "order_base.h"
#include "order_cmd.h"
#include "orderlist_edit.h"
#include "orderlist_gui.h"
#include "tilehighlight_func.h"
#include "command_func.h"
#include "company_func.h"
#include "company_base.h"
#include "company_gui.h"
#include "dropdown_func.h"
#include "dropdown_type.h"
#include <map>
#include <set>
#include "window_gui.h"
#include "window_func.h"
#include "gfx_func.h"
#include "zoom_func.h"
#include "textbuf_gui.h"
#include "strings_func.h"
#include "string_func.h"
#include "settings_gui.h"
#include "openttd.h"
#include "sortlist_type.h"
#include "stringfilter_type.h"
#include "querystring_gui.h"
#include "core/pool_func.hpp"
#include "core/geometry_func.hpp"
#include "widgets/orderlist_widget.h"
#include "table/sprites.h"
#include "table/strings.h"

static constexpr NWidgetPart _nested_orderlist_widgets[] = {
	NWidget(NWID_HORIZONTAL),
		NWidget(WWT_CLOSEBOX, Colours::Grey),
		NWidget(WWT_CAPTION, Colours::Grey, WID_OL_CAPTION), SetStringTip(STR_ORDER_LIST_CAPTION, STR_NULL),
		NWidget(WWT_SHADEBOX, Colours::Grey),
		NWidget(WWT_DEFSIZEBOX, Colours::Grey),
		NWidget(WWT_STICKYBOX, Colours::Grey),
	EndContainer(),
	NWidget(NWID_HORIZONTAL),
		NWidget(WWT_TEXTBTN, Colours::Grey, WID_OL_SORT_ORDER), SetStringTip(STR_BUTTON_SORT_BY, STR_TOOLTIP_SORT_ORDER),
		NWidget(WWT_DROPDOWN, Colours::Grey, WID_OL_SORT_CRITERIA), SetToolTip(STR_TOOLTIP_SORT_CRITERIA),
		NWidget(WWT_EDITBOX, Colours::Grey, WID_OL_FILTER), SetFill(1, 0), SetResize(1, 0), SetStringTip(STR_LIST_FILTER_OSKTITLE, STR_LIST_FILTER_TOOLTIP),
	EndContainer(),
	NWidget(WWT_TEXTBTN, Colours::Grey, WID_OL_UNGROUPED), SetFill(1, 0), SetStringTip(STR_ORDER_LIST_UNGROUPED, STR_ORDER_LIST_MOVE_GROUP_TOOLTIP),

	NWidget(NWID_HORIZONTAL),
		NWidget(WWT_PANEL, Colours::Grey),
			NWidget(NWID_HORIZONTAL),
				NWidget(WWT_INSET, Colours::Grey, WID_OL_LIST), SetFill(1, 1), SetPadding(2, 1, 2, 2), SetResize(1, 1), SetScrollbar(WID_OL_SCROLLBAR), SetToolTip(STR_ORDER_LIST_LIST_TOOLTIP),
				EndContainer(),
			EndContainer(),
		EndContainer(),
		NWidget(NWID_VSCROLLBAR, Colours::Grey, WID_OL_SCROLLBAR),
	EndContainer(),

	NWidget(NWID_HORIZONTAL),
		NWidget(WWT_PUSHTXTBTN, Colours::Grey, WID_OL_NEW_GROUP), SetResize(1, 0), SetFill(1, 0), SetStringTip(STR_ORDER_LIST_NEW_GROUP, STR_ORDER_LIST_NEW_GROUP_TOOLTIP),
		NWidget(WWT_DROPDOWN, Colours::Grey, WID_OL_MOVE_GROUP), SetResize(1, 0), SetFill(1, 0), SetStringTip(STR_ORDER_LIST_MOVE_GROUP, STR_ORDER_LIST_MOVE_GROUP_TOOLTIP),
	EndContainer(),
	NWidget(WWT_PANEL, Colours::Grey),
		NWidget(NWID_HORIZONTAL),
			NWidget(NWID_HORIZONTAL, NWidContainerFlag::EqualSize),
				NWidget(WWT_PUSHTXTBTN, Colours::Grey, WID_OL_NEW), SetResize(1, 0), SetFill(1, 0), SetStringTip(STR_ORDER_LIST_NEW, STR_ORDER_LIST_NEW_TOOLTIP),
				NWidget(WWT_PUSHTXTBTN, Colours::Grey, WID_OL_RENAME), SetResize(1, 0), SetFill(1, 0), SetStringTip(STR_BUTTON_RENAME, STR_NULL),
				NWidget(WWT_TEXTBTN, Colours::Grey, WID_OL_PUBLIC), SetResize(1, 0), SetFill(1, 0), SetToolTip(STR_ORDER_LIST_PUBLIC_TOOLTIP),
				NWidget(WWT_PUSHTXTBTN, Colours::Grey, WID_OL_DELETE), SetResize(1, 0), SetFill(1, 0), SetStringTip(STR_ORDER_LIST_DELETE, STR_ORDER_LIST_DELETE_TOOLTIP),
			EndContainer(),
			NWidget(WWT_RESIZEBOX, Colours::Grey),
		EndContainer(),
	EndContainer(),
};

static WindowDesc _orderlist_desc(__FILE__, __LINE__,
	WindowPosition::Automatic, "order_list_manager", 350, 100,
	WindowClass::OrderList, WindowClass::None,
	WindowDefaultFlag::Construction,
	_nested_orderlist_widgets
);

typedef GUIList<const OrderList *, const bool &> GUIOrderList;

struct OrderListWindow : Window {
	Scrollbar *vscroll = nullptr;                   ///< Vertical scrollbar of the list of order lists.
	WidgetID query_widget = WID_OL_CAPTION;         ///< Currently open query widget (rename dialog).
	std::vector<OrderListID> list{};                ///< The translation table linking panel indices to their related OrderListID.
	int selected = INT_MAX;                         ///< What item is currently selected in the panel.
	uint edit_btn_left = 0;                         ///< left offset of the edit buttons
	Dimension company_icon_spr_dim{};               ///< dimensions of company icon
	Dimension edit_btn_spr_dim{};                   ///< dimensions of the edit button sprite
	std::vector<uint> depths;                       ///< Indentation of each displayed row.
	std::set<OrderListID> collapsed;                ///< Local folder expansion state, never simulation state.
	std::vector<OrderListID> move_targets;
	OrderListID query_target = OrderListID::Invalid();
	OrderListID query_parent = OrderListID::Invalid();
	OrderListID move_source = OrderListID::Invalid();
	OrderListID drag_source = OrderListID::Invalid();
	OrderListID drag_target = OrderListID::Invalid();
	bool drag_root = false;

private:
	/* Runtime saved values */
	static Listing last_sorting;

	/* Constants for sorting order lists */
	static inline const StringID sorter_names[] = {
		STR_SORT_BY_ORDER_LIST_ID,
		STR_SORT_BY_NAME,
		STR_SORT_BY_OWNER,
	};
	static const std::initializer_list<GUIOrderList::SortFunction * const> sorter_funcs;

	StringFilter string_filter;             ///< Filter for order lists
	QueryString orderlist_editbox;          ///< Filter editbox

	GUIOrderList orders{OrderListWindow::last_sorting.order};

	void BuildSortOrderList()
	{
		if (this->orders.NeedRebuild()) {
			this->orders.clear();
			this->orders.reserve(OrderList::GetNumItems());

			for (const OrderList *ol : OrderList::Iterate()) {
				if (ol->IsVisibleToCompany(_local_company) || (ol->IsScheduleGroup() && ol->GetCompany() == _local_company)) this->orders.push_back(ol);
			}

			this->orders.RebuildDone();
			this->SetDirty();
		}
		/* Always sort the order lists. */
		this->orders.Sort();
		this->SetWidgetDirty(WID_OL_LIST); // Force repaint of the displayed order lists.

		this->RebuildList();
	}

	void RebuildList()
	{
		std::optional<OrderListID> previously_selected;
		if (this->selected >= 0 && this->selected < (int)this->list.size()) previously_selected = this->list[this->selected];

		this->selected = INT_MAX;

		this->list.clear();
		this->depths.clear();
		std::map<OrderListID, std::vector<const OrderList *>> children;
		std::set<OrderListID> matches;
		for (const OrderList *ol : this->orders) {
			/* Other companies' public schedules are visible at the root. */
			OrderListID parent = ol->GetCompany() == _local_company ? ol->GetScheduleGroup() : OrderListID::Invalid();
			children[parent].push_back(ol);
			if (!this->string_filter.IsEmpty()) {
				this->string_filter.ResetState();
				this->string_filter.AddLine(ol->GetName());
				if (this->string_filter.GetState()) {
					matches.insert(ol->index);
					while (parent != OrderListID::Invalid() && matches.insert(parent).second) parent = OrderList::Get(parent)->GetScheduleGroup();
				}
			}
		}
		std::vector<std::pair<const OrderList *, uint>> pending;
		auto append_children = [&](OrderListID parent, uint depth) {
			auto &items = children[parent];
			std::stable_partition(items.begin(), items.end(), [](const OrderList *ol) { return ol->IsScheduleGroup(); });
			for (auto it = items.rbegin(); it != items.rend(); ++it) pending.emplace_back(*it, depth);
		};
		append_children(OrderListID::Invalid(), 0);
		while (!pending.empty()) {
			auto [ol, depth] = pending.back();
			pending.pop_back();
			if (!this->string_filter.IsEmpty() && !matches.contains(ol->index)) continue;
			this->list.push_back(ol->index);
			this->depths.push_back(depth);
			if (previously_selected == ol->index) this->selected = (int)this->list.size() - 1;
			if (ol->IsScheduleGroup() && (!this->collapsed.contains(ol->index) || !this->string_filter.IsEmpty())) append_children(ol->index, depth + 1);
		}

		this->vscroll->SetCount((int) this->list.size());
	}

	bool CanDropOn(OrderListID target) const
	{
		const OrderList *source = OrderList::GetIfValid(this->drag_source);
		const OrderList *group = OrderList::GetIfValid(target);
		if (source == nullptr || source->GetCompany() != _local_company) return false;
		if (target == OrderListID::Invalid()) return source->GetScheduleGroup() != target;
		if (group == nullptr || !group->IsScheduleGroup() || group->GetCompany() != _local_company || source->GetScheduleGroup() == target) return false;
		for (const OrderList *ancestor = group; ancestor != nullptr; ancestor = OrderList::GetIfValid(ancestor->GetScheduleGroup())) {
			if (ancestor == source) return false;
		}
		return true;
	}

	/** Sort by order list ID */
	static bool OrderListIDSorter(const OrderList * const &a, const OrderList * const &b, const bool &)
	{
		return a->index < b->index;
	}

	/** Sort by order list name */
	static bool OrderListNameSorter(const OrderList * const &a, const OrderList * const &b, const bool &)
	{
		int r = StrNaturalCompare(a->GetName(), b->GetName());
		return r != 0 ? r < 0 : a->index < b->index;
	}

	/** Sort by order list owner */
	static bool OrderListOwnerSorter(const OrderList * const &a, const OrderList * const &b, const bool &order)
	{
		if (a->GetCompany() == b->GetCompany()) return OrderListNameSorter(a, b, order);
		return a->GetCompany() < b->GetCompany();
	}

public:
	OrderListWindow(WindowDesc &desc) : Window(desc), orderlist_editbox(MAX_LENGTH_ORDERLIST_NAME_CHARS * MAX_CHAR_LENGTH, MAX_LENGTH_ORDERLIST_NAME_CHARS)
	{
		this->CreateNestedTree();
		this->vscroll = this->GetScrollbar(WID_OL_SCROLLBAR);
		this->FinishInitNested();

		this->orders.SetListing(this->last_sorting);
		this->orders.SetSortFuncs(OrderListWindow::sorter_funcs);
		this->orders.ForceRebuild();
		this->BuildSortOrderList();

		this->querystrings[WID_OL_FILTER] = &this->orderlist_editbox;
		this->orderlist_editbox.cancel_button = QueryString::ACTION_CLEAR;
	}

	virtual void OnClick(Point pt, WidgetID widget, int click_count) override
	{
		switch (widget) {
			case WID_OL_NEW:
				{
					Command<Commands::CreateOrderList>::Post(CommandCallback::CreateOrderList, TileIndex{}, std::string{}, this->SelectedParent(), false);
				}
				break;
			case WID_OL_NEW_GROUP:
				this->query_widget = widget;
				this->query_parent = this->SelectedParent();
				ShowQueryString(std::string{}, STR_ORDER_LIST_NEW_GROUP, MAX_LENGTH_ORDERLIST_NAME_CHARS, this, CS_ALPHANUMERAL, QueryStringFlag::LengthIsInChars);
				break;
			case WID_OL_MOVE_GROUP:
				if (const OrderList *ol = this->GetSelectedOrderList(); ol != nullptr && ol->GetCompany() == _local_company) {
					this->move_source = ol->index;
					this->move_targets = {OrderListID::Invalid()};
					DropDownList items;
					items.push_back(MakeDropDownListStringItem(GetString(STR_ORDER_LIST_UNGROUPED), 0, false));
					int selected_parent = 0;
					for (const OrderList *group : this->orders) {
						if (!group->IsScheduleGroup()) continue;
						bool descendant = false;
						for (const OrderList *ancestor = group; ancestor != nullptr; ancestor = OrderList::GetIfValid(ancestor->GetScheduleGroup())) {
							if (ancestor == ol) { descendant = true; break; }
						}
						if (descendant) continue;
						int index = (int)this->move_targets.size();
						if (group->index == ol->GetScheduleGroup()) selected_parent = index;
						this->move_targets.push_back(group->index);
						items.push_back(MakeDropDownListStringItem(this->GroupPath(group), index, false));
					}
					ShowDropDownList(this, std::move(items), selected_parent, widget);
				}
				break;
			case WID_OL_UNGROUPED:
				this->selected = INT_MAX;
				this->SetDirty();
				break;

			case WID_OL_RENAME:
				if (const OrderList *ol = this->GetSelectedOrderList(); ol != nullptr) {
					this->query_widget = WID_OL_RENAME;
					this->query_target = ol->index;
					ShowQueryString(GetString(STR_JUST_RAW_STRING, ol->GetName()),
							STR_ORDER_LIST_QUERY_RENAME, MAX_LENGTH_ORDERLIST_NAME_CHARS,
							this, CS_ALPHANUMERAL, QueryStringFlag::LengthIsInChars);
				}
				break;

			case WID_OL_PUBLIC:
				if (const OrderList *ol = this->GetSelectedOrderList(); ol != nullptr) {
					Command<Commands::SetOrderListPublic>::Post(this->list[this->selected], !ol->IsPublic());
				}
				break;

			case WID_OL_DELETE:
				if (this->selected != INT_MAX) {
					Command<Commands::DeleteOrderList>::Post(STR_ERROR_CAN_T_DELETE_ORDER_LIST, this->list[this->selected]);
				}
				break;

			case WID_OL_LIST: {
				int new_selected = this->vscroll->GetScrolledRowFromWidget(pt.y, this, WID_OL_LIST, WidgetDimensions::scaled.framerect.top);
				bool opened_editor = false;
				if (new_selected >= 0 && new_selected < (int)this->list.size()) {
					const OrderList *ol = OrderList::Get(this->list[new_selected]);
					const Rect inner = this->GetWidget<NWidgetBase>(WID_OL_LIST)->GetCurrentRect().Shrink(WidgetDimensions::scaled.framerect);
					const bool rtl = _current_text_dir == TD_RTL;
					const int text_left = rtl ? inner.left : this->edit_btn_left + SETTING_BUTTON_WIDTH + 4;
					const int text_right = rtl ? this->edit_btn_left - 4 : inner.right;
					const int indent = std::min<uint>(this->depths[new_selected] * ScaleGUITrad(12), (text_right - text_left) / 2);
					const int fold_left = rtl ? text_right - indent - GetSpriteSize(SPR_CIRCLE_FOLDED).width : text_left + indent;
					if (ol->IsScheduleGroup() && (click_count > 1 || (pt.x >= fold_left && pt.x < fold_left + ScaleGUITrad(14)))) {
						this->selected = new_selected;
						if (!this->collapsed.erase(ol->index)) this->collapsed.insert(ol->index);
						this->RebuildList();
						this->SetDirty();
						break;
					}
					const int btn_left = this->edit_btn_left;
					const int btn_right = btn_left + SETTING_BUTTON_WIDTH;
					if (!ol->IsScheduleGroup() && pt.x >= btn_left && pt.x < btn_right) {
						ShowOrderListEditor(this->list[new_selected]);
						opened_editor = true;
					}
				}
				this->selected = new_selected >= 0 && new_selected < (int)this->list.size() ? new_selected : INT_MAX;
				if (const OrderList *selected = this->GetSelectedOrderList(); !opened_editor && selected != nullptr && selected->GetCompany() == _local_company) {
					this->drag_source = selected->index;
					SetObjectToPlaceWnd(SPR_CURSOR_MOUSE, PAL_NONE, HT_DRAG, this);
				}
				this->SetDirty();
				break;
			}

			case WID_OL_SORT_ORDER: // Click on sort order button
				this->orders.ToggleSortOrder();
				this->orders.ForceResort();
				this->BuildSortOrderList();
				this->SetWidgetDirty(WID_OL_SORT_ORDER);
				break;

			case WID_OL_SORT_CRITERIA: // Click on sort criteria dropdown
				ShowDropDownMenu(this, OrderListWindow::sorter_names, this->orders.SortType(), WID_OL_SORT_CRITERIA, 0, 0);
				break;

			default: break;
		}
	}

	void OnMouseDrag(Point pt, WidgetID widget) override
	{
		OrderListID target = OrderListID::Invalid();
		bool root = widget == WID_OL_UNGROUPED && this->CanDropOn(OrderListID::Invalid());
		if (widget == WID_OL_LIST) {
			int row = this->vscroll->GetScrolledRowFromWidget(pt.y, this, WID_OL_LIST, WidgetDimensions::scaled.framerect.top);
			if (row >= 0 && row < (int)this->list.size() && this->CanDropOn(this->list[row])) target = this->list[row];
		}
		if (target != this->drag_target || root != this->drag_root) {
			this->drag_target = target;
			this->drag_root = root;
			this->SetWidgetDirty(WID_OL_LIST);
			this->SetWidgetDirty(WID_OL_UNGROUPED);
		}
	}

	void OnDragDrop(Point pt, WidgetID widget) override
	{
		if (widget == WID_OL_LIST) {
			int row = this->vscroll->GetScrolledRowFromWidget(pt.y, this, WID_OL_LIST, WidgetDimensions::scaled.framerect.top);
			if (row >= 0 && row < (int)this->list.size() && this->CanDropOn(this->list[row])) {
				Command<Commands::MoveOrderListToGroup>::Post(this->drag_source, this->list[row]);
			}
		} else if (widget == WID_OL_UNGROUPED && this->CanDropOn(OrderListID::Invalid())) {
			Command<Commands::MoveOrderListToGroup>::Post(this->drag_source, OrderListID::Invalid());
		}
		this->drag_source = OrderListID::Invalid();
		this->drag_target = OrderListID::Invalid();
		this->drag_root = false;
		this->SetDirty();
	}

	virtual void OnDropdownSelect(WidgetID widget, int index, int) override
	{
		switch (widget) {
			case WID_OL_MOVE_GROUP:
				if (index >= 0 && index < (int)this->move_targets.size()) Command<Commands::MoveOrderListToGroup>::Post(this->move_source, this->move_targets[index]);
				break;
			case WID_OL_SORT_CRITERIA:
				if (this->orders.SortType() != index) {
					this->orders.SetSortType(index);
					this->last_sorting = this->orders.GetListing(); // Store new sorting order.
					this->BuildSortOrderList();
				}
				break;
		}
	}

	void OnQueryTextFinished(std::optional<std::string> str) override
	{
		WidgetID query = this->query_widget;
		this->query_widget = WID_OL_CAPTION;
		if (!str.has_value()) return;
		if (query == WID_OL_NEW_GROUP && !str->empty()) {
			Command<Commands::CreateOrderList>::Post(TileIndex{}, *str, this->query_parent, true);
		} else if (query == WID_OL_RENAME) {
			Command<Commands::RenameOrderList>::Post(STR_ERROR_CAN_T_RENAME_ORDER_LIST, TileIndex{}, this->query_target, *str);
		}
	}

	OrderListID SelectedParent() const
	{
		const OrderList *ol = this->GetSelectedOrderList();
		if (ol == nullptr || ol->GetCompany() != _local_company) return OrderListID::Invalid();
		return ol->IsScheduleGroup() ? ol->index : ol->GetScheduleGroup();
	}

	std::string GroupPath(const OrderList *group) const
	{
		std::string path = group->GetName();
		for (const OrderList *parent = OrderList::GetIfValid(group->GetScheduleGroup()); parent != nullptr; parent = OrderList::GetIfValid(parent->GetScheduleGroup())) path = parent->GetName() + " / " + path;
		return path;
	}

	const OrderList *GetSelectedOrderList() const
	{
		if (this->selected < 0 || this->selected >= (int) this->list.size()) return nullptr;
		OrderListID id = this->list[this->selected];
		return OrderList::IsValidID(id) ? OrderList::Get(id) : nullptr;
	}

	virtual void OnPaint() override
	{
		bool no_company = _game_mode != GameMode::Editor && !Company::IsValidID(_local_company);
		this->SetWidgetDisabledState(WID_OL_NEW, no_company);
		this->SetWidgetDisabledState(WID_OL_NEW_GROUP, no_company);
		this->SetWidgetLoweredState(WID_OL_UNGROUPED, this->drag_root);

		const OrderList *ol = this->GetSelectedOrderList();
		if (ol != nullptr && ol->GetCompany() == _local_company) {
			this->SetWidgetsDisabledState(false, WID_OL_RENAME, WID_OL_MOVE_GROUP, WID_OL_PUBLIC, WID_OL_DELETE);
			this->GetWidget<NWidgetCore>(WID_OL_PUBLIC)->SetString(ol->IsPublic() ? STR_ORDER_LIST_MAKE_PRIVATE : STR_ORDER_LIST_MAKE_PUBLIC);
			/* A public list may already be used by other companies; making it
			 * private again has no effect. Keep the button as a placeholder, but disabled. */
			this->SetWidgetDisabledState(WID_OL_PUBLIC, ol->IsPublic() || ol->IsScheduleGroup());
		} else {
			this->SetWidgetsDisabledState(true, WID_OL_RENAME, WID_OL_MOVE_GROUP, WID_OL_PUBLIC, WID_OL_DELETE);
			this->GetWidget<NWidgetCore>(WID_OL_PUBLIC)->SetString(STR_ORDER_LIST_MAKE_PUBLIC);
		}
		this->DrawWidgets();
	}

	virtual void DrawWidget(const Rect &r, WidgetID widget) const override
	{
		switch (widget) {
			case WID_OL_SORT_ORDER:
				this->DrawSortButton(widget, this->orders.IsDescSortOrder());
				break;

			case WID_OL_LIST: {
				Rect ir = r.Shrink(WidgetDimensions::scaled.framerect);
				uint y = ir.top; // Offset from top of widget.
				if (this->vscroll->GetCount() == 0) {
					DrawString(ir.left, ir.right, y, STR_STATION_LIST_NONE);
					return;
				}

				bool rtl = _current_text_dir == TD_RTL;
				uint icon_left  = (rtl ? ir.right - this->company_icon_spr_dim.width : r.left);
				uint btn_left   = (rtl ? icon_left - SETTING_BUTTON_WIDTH - 4 : icon_left + this->company_icon_spr_dim.width + 4);
				uint text_left  = (rtl ? ir.left : btn_left + SETTING_BUTTON_WIDTH + 4);
				uint text_right = (rtl ? btn_left - 4 : ir.right);
				const_cast<OrderListWindow*>(this)->edit_btn_left = btn_left;

				for (int32_t i = this->vscroll->GetPosition(); this->vscroll->IsVisible(i) && i < this->vscroll->GetCount(); i++) {
					OrderListID id = this->list[i];

					if (i == this->selected || id == this->drag_target) GfxFillRect(r.left + 1, y, r.right, y + this->resize.step_height, id == this->drag_target ? PC_LIGHT_BLUE : PC_DARK_GREY);

					if (OrderList::IsValidID(id)) {
						const OrderList *ol = OrderList::Get(id);

						if (Company::IsValidID(ol->GetCompany())) {
							DrawCompanyIcon(ol->GetCompany(), icon_left, y + (this->resize.step_height - this->company_icon_spr_dim.height) / 2);
						}

						Rect br{(int) btn_left, (int) y + ((int) this->resize.step_height - SETTING_BUTTON_HEIGHT) / 2,
								(int) btn_left + SETTING_BUTTON_WIDTH - 1, (int) y + ((int) this->resize.step_height + SETTING_BUTTON_HEIGHT) / 2 - 1};
						if (!ol->IsScheduleGroup()) {
							DrawFrameRect(br, Company::IsValidID(ol->GetCompany()) ? _company_colours[ol->GetCompany()] : Colours::Grey, {});
							DrawSprite(SPR_RENAME, PAL_NONE, CentreBounds(br.left, br.right, this->edit_btn_spr_dim.width), CentreBounds(br.top, br.bottom, this->edit_btn_spr_dim.height));
						}

						std::string base_name = ol->GetName().empty() ? GetString(STR_ORDER_LIST_DEFAULT_NAME, ol->index.base() + 1) : ol->GetName();
						std::string str = ol->IsScheduleGroup() || ol->IsPublic() ? base_name : GetString(STR_PLANS_LIST_ITEM_PLAN_PRIVATE) + base_name;
						int indent = std::min<uint>(this->depths[i] * ScaleGUITrad(12), (text_right - text_left) / 2);
						if (ol->IsScheduleGroup()) {
							SpriteID sprite = this->collapsed.contains(id) && this->string_filter.IsEmpty() ? SPR_CIRCLE_FOLDED : SPR_CIRCLE_UNFOLDED;
							Dimension dim = GetSpriteSize(sprite);
							DrawSprite(sprite, PAL_NONE, rtl ? text_right - indent - dim.width : text_left + indent, y + (this->resize.step_height - dim.height) / 2);
							indent += std::max<uint>(ScaleGUITrad(14), dim.width + WidgetDimensions::scaled.hsep_normal);
						}
						DrawString(text_left + (rtl ? 0 : indent), text_right - (rtl ? indent : 0), y + (this->resize.step_height - GetCharacterHeight(FontSize::Normal)) / 2, str, TextColour::White);
					}
					y += this->resize.step_height;
				}
				break;
			}
		}
	}

	std::string GetWidgetString(WidgetID widget, StringID stringid) const override
	{
		switch (widget) {
			case WID_OL_SORT_CRITERIA:
				return GetString(OrderListWindow::sorter_names[this->orders.SortType()]);

			default:
				return this->Window::GetWidgetString(widget, stringid);
		}
	}

	virtual void OnResize() override
	{
		this->vscroll->SetCapacityFromWidget(this, WID_OL_LIST, WidgetDimensions::scaled.framerect.Vertical());
	}

	virtual void UpdateWidgetSize(WidgetID widget, Dimension &size, const Dimension &padding, Dimension &fill, Dimension &resize) override
	{
		switch (widget) {
			case WID_OL_SORT_ORDER: {
				Dimension d = GetStringBoundingBox(this->GetWidget<NWidgetCore>(widget)->GetString());
				d.width += padding.width + Window::SortButtonWidth() * 2; // Doubled since the string is centred and it also looks better.
				d.height += padding.height;
				size = maxdim(size, d);
				break;
			}

			case WID_OL_SORT_CRITERIA: {
				Dimension d = GetStringListBoundingBox(OrderListWindow::sorter_names);
				d.width += padding.width;
				d.height += padding.height;
				size = maxdim(size, d);
				break;
			}

			case WID_OL_LIST:
				this->company_icon_spr_dim = GetSpriteSize(SPR_COMPANY_ICON);
				this->edit_btn_spr_dim = GetSpriteSize(SPR_RENAME);
				resize.height = std::max<int>(GetCharacterHeight(FontSize::Normal), SETTING_BUTTON_HEIGHT);
				size.height = resize.height * 5 + WidgetDimensions::scaled.framerect.Vertical();
				break;

			case WID_OL_NEW:
				size = adddim(GetStringBoundingBox(STR_ORDER_LIST_NEW), padding);
				break;

			case WID_OL_RENAME:
				size = adddim(GetStringBoundingBox(STR_BUTTON_RENAME), padding);
				break;

			case WID_OL_PUBLIC:
				size = adddim(maxdim(GetStringBoundingBox(STR_ORDER_LIST_MAKE_PUBLIC), GetStringBoundingBox(STR_ORDER_LIST_MAKE_PRIVATE)), padding);
				break;

			case WID_OL_DELETE:
				size = adddim(GetStringBoundingBox(STR_ORDER_LIST_DELETE), padding);
				break;
		}
	}

	void OnEditboxChanged(WidgetID wid) override
	{
		if (wid == WID_OL_FILTER) {
			this->string_filter.SetFilterTerm(this->orderlist_editbox.text.GetText());
			this->orders.ForceRebuild();
			this->BuildSortOrderList();
		}
	}

	virtual void OnInvalidateData(int data = 0, bool gui_scope = true) override
	{
		if (data != 0 && this->selected >= 0 && this->selected < (int)this->list.size()) {
			if (this->list[this->selected].base() == data) {
				/* Invalidate the selection if the selected order list has been deleted. */
				this->selected = INT_MAX;
			}
		}

		this->orders.ForceRebuild();
		this->BuildSortOrderList();
	}
};

Listing OrderListWindow::last_sorting = {false, 0};

/** Available order list sorting functions. */
const std::initializer_list<GUIOrderList::SortFunction * const> OrderListWindow::sorter_funcs = {
	&OrderListIDSorter,
	&OrderListNameSorter,
	&OrderListOwnerSorter,
};

/**
 * Callback after creating a new order list; opens the editor for it.
 */
void CcCreateOrderList(const CommandCost &result, [[maybe_unused]] const std::string &name, [[maybe_unused]] OrderListID parent, bool is_group)
{
	if (!result.Succeeded() || is_group) return;

	auto id = result.GetResultData<OrderListID>();
	ShowOrderListEditor(*id);
}

/** Display string for a list name: falls back to a default name when unset. */
static std::string GetOrderListDisplayName(const OrderList *ol)
{
	if (ol->GetName().empty()) return GetString(STR_ORDER_LIST_DEFAULT_NAME, ol->index.base() + 1);
	return ol->GetName();
}

/** Show the window to manage player-created order lists. */
void ShowOrderListManager()
{
	if (BringWindowToFrontById(WindowClass::OrderList, 0) != nullptr) return;
	new OrderListWindow(_orderlist_desc);
}
