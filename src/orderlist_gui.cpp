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
#include "command_func.h"
#include <cstdio>
#include "company_func.h"
#include "company_base.h"
#include "company_gui.h"
#include "dropdown_func.h"
#include "window_gui.h"
#include "window_func.h"
#include "gfx_func.h"
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

	NWidget(NWID_HORIZONTAL),
		NWidget(WWT_PANEL, Colours::Grey),
			NWidget(NWID_HORIZONTAL),
				NWidget(WWT_INSET, Colours::Grey, WID_OL_LIST), SetFill(1, 1), SetPadding(2, 1, 2, 2), SetResize(1, 0), SetScrollbar(WID_OL_SCROLLBAR), SetToolTip(STR_ORDER_LIST_LIST_TOOLTIP),
				EndContainer(),
			EndContainer(),
		EndContainer(),
		NWidget(NWID_VSCROLLBAR, Colours::Grey, WID_OL_SCROLLBAR),
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
		fprintf(stderr, "[OL][manager] build needRebuild=%d local=%u pool=%zu\n",
				this->orders.NeedRebuild() ? 1 : 0, (unsigned)_local_company.base(), (size_t)OrderList::GetNumItems());
		if (this->orders.NeedRebuild()) {
			this->orders.clear();
			this->orders.reserve(OrderList::GetNumItems());

			for (const OrderList *ol : OrderList::Iterate()) {
				fprintf(stderr, "[OL][manager]   list %u: player=%d vis=%d name='%s'\n",
						ol->index.base(), ol->IsPlayerCreated(), ol->IsVisibleToCompany(_local_company), ol->GetName().c_str());
				if (!ol->IsVisibleToCompany(_local_company)) continue;
				if (this->string_filter.IsEmpty()) {
					this->orders.push_back(ol);
				} else {
					this->string_filter.ResetState();
					this->string_filter.AddLine(ol->GetName());
					if (this->string_filter.GetState()) this->orders.push_back(ol);
				}
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
		if (this->selected != INT_MAX) previously_selected = this->list[this->selected];

		this->selected = INT_MAX;

		this->list.clear();
		for (const OrderList *ol : this->orders) {
			this->list.push_back(ol->index);
			if (previously_selected == ol->index) this->selected = (int) this->list.size() - 1;
		}

		this->vscroll->SetCount((int) this->list.size());
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
				fprintf(stderr, "[OL][ui] new-button clicked, posting CreateOrderList\n");
				{
					bool posted = Command<Commands::CreateOrderList>::Post(CommandCallback::CreateOrderList, TileIndex{}, {});
					fprintf(stderr, "[OL][ui] post returned %d\n", posted ? 1 : 0);
				}
				break;

			case WID_OL_RENAME:
				if (const OrderList *ol = this->GetSelectedOrderList(); ol != nullptr) {
					this->query_widget = WID_OL_RENAME;
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
					Command<Commands::DeleteOrderList>::Post(this->list[this->selected]);
				}
				break;

			case WID_OL_LIST: {
				int new_selected = this->vscroll->GetScrolledRowFromWidget(pt.y, this, WID_OL_LIST, WidgetDimensions::scaled.framerect.top);
				if (new_selected != INT_MAX) {
					const int btn_left = this->edit_btn_left;
					const int btn_right = btn_left + SETTING_BUTTON_WIDTH;
					if (pt.x >= btn_left && pt.x < btn_right) {
						if (new_selected >= 0 && static_cast<size_t>(new_selected) < this->list.size()) ShowOrderListEditor(this->list[new_selected]);
					}
				}
				this->selected = new_selected;
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

	virtual void OnDropdownSelect(WidgetID widget, int index, int) override
	{
		switch (widget) {
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
		if (!str.has_value() || this->query_widget != WID_OL_RENAME) return;
		this->query_widget = {};

		if (this->selected == INT_MAX || this->selected >= static_cast<int>(this->list.size())) return;
		OrderListID id = this->list[this->selected];

		Command<Commands::RenameOrderList>::Post(STR_ERROR_CAN_T_RENAME_ORDER_LIST, TileIndex{}, id, *str);
	}

	const OrderList *GetSelectedOrderList() const
	{
		if (this->selected == INT_MAX || this->selected >= (int) this->list.size()) return nullptr;
		OrderListID id = this->list[this->selected];
		return OrderList::IsValidID(id) ? OrderList::Get(id) : nullptr;
	}

	virtual void OnPaint() override
	{
		bool no_company = _game_mode != GameMode::Editor && !Company::IsValidID(_local_company);
		this->SetWidgetDisabledState(WID_OL_NEW, no_company);

		const OrderList *ol = this->GetSelectedOrderList();
		if (ol != nullptr && ol->GetCompany() == _local_company) {
			this->SetWidgetsDisabledState(false, WID_OL_RENAME, WID_OL_PUBLIC, WID_OL_DELETE);
			this->GetWidget<NWidgetCore>(WID_OL_PUBLIC)->SetString(ol->IsPublic() ? STR_ORDER_LIST_MAKE_PRIVATE : STR_ORDER_LIST_MAKE_PUBLIC);
		} else {
			this->SetWidgetsDisabledState(true, WID_OL_RENAME, WID_OL_PUBLIC, WID_OL_DELETE);
			this->GetWidget<NWidgetCore>(WID_OL_PUBLIC)->SetString(STR_ORDER_LIST_MAKE_PUBLIC);
		}
		this->DrawWidgets();
	}

	virtual void DrawWidget(const Rect &r, WidgetID widget) const override
	{
		switch (widget) {
			case WID_OL_SORT_ORDER:
				this->DrawSortButtonState(widget, this->orders.IsDescSortOrder() ? SBS_DOWN : SBS_UP);
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

					if (i == this->selected) GfxFillRect(r.left + 1, y, r.right, y + this->resize.step_height, PC_DARK_GREY);

					if (OrderList::IsValidID(id)) {
						const OrderList *ol = OrderList::Get(id);

						if (Company::IsValidID(ol->GetCompany())) {
							DrawCompanyIcon(ol->GetCompany(), icon_left, y + (this->resize.step_height - this->company_icon_spr_dim.height) / 2);
						}

						Rect br{(int) btn_left, (int) y + ((int) this->resize.step_height - SETTING_BUTTON_HEIGHT) / 2,
								(int) btn_left + SETTING_BUTTON_WIDTH - 1, (int) y + ((int) this->resize.step_height + SETTING_BUTTON_HEIGHT) / 2 - 1};
						DrawFrameRect(br, Colours::Grey, {});
						DrawSprite(SPR_RENAME, PAL_NONE, CentreBounds(br.left, br.right, this->edit_btn_spr_dim.width), CentreBounds(br.top, br.bottom, this->edit_btn_spr_dim.height));

						std::string str = ol->IsPublic() ? ol->GetName() : GetString(STR_PLANS_LIST_ITEM_PLAN_PRIVATE) + ol->GetName();
						DrawString(text_left, text_right, y + (this->resize.step_height - GetCharacterHeight(FontSize::Normal)) / 2, str, TextColour::White);
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
		if (data != 0 && this->selected != INT_MAX) {
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
void CcCreateOrderList(const CommandCost &result, [[maybe_unused]] const std::string &name)
{
	fprintf(stderr, "[OL][cc-create] fired ok=%d\n", result.Succeeded());
	if (!result.Succeeded()) return;

	auto id = result.GetResultData<OrderListID>();
	if (!id.has_value()) { fprintf(stderr, "[OL][cc-create] no id in result\n"); return; }
	fprintf(stderr, "[OL][cc-create] opening editor for list %u\n", id->base());
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
