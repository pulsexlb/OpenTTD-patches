/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file orderlist_widget.h Types related to the order list manager widgets. */

#ifndef WIDGETS_ORDERLIST_WIDGET_H
#define WIDGETS_ORDERLIST_WIDGET_H

/** Widgets of the #OrderListWindow class. */
enum OrderListWidgets : WidgetID {
	WID_OL_CAPTION,         ///< Caption of the window.
	WID_OL_SORT_ORDER,      ///< Direction of sort dropdown.
	WID_OL_SORT_CRITERIA,   ///< Criteria of sort dropdown.
	WID_OL_FILTER,          ///< Filter of name.
	WID_OL_LIST,            ///< List of order lists.
	WID_OL_SCROLLBAR,       ///< Scrollbar of the list.
	WID_OL_NEW,             ///< Create a new order list.
	WID_OL_RENAME,          ///< Rename the selected order list.
	WID_OL_PUBLIC,          ///< Make the selected order list public/private.
	WID_OL_DELETE,          ///< Delete the selected order list.
};

#endif /* WIDGETS_ORDERLIST_WIDGET_H */
