/* Actual OrdersWindow callbacks, native schedule commands and real loaded games.
 * The observer never changes the production GUI implementation. */
#include "orderlist_edit.h"
#include "order_base.h"
#include "order_cmd.h"
#include "orderlist_gui.h"
#include "widgets/order_widget.h"
#include "waypoint_cmd.h"
#include "newgrf_station.h"
#include "station_map.h"
#include "company_base.h"
#include "company_func.h"
#include "train.h"
#include "gui.h"
#include "window_gui.h"
#include "window_func.h"
#include "tilehighlight_func.h"
#include "strings_func.h"
#include <windows.h>

namespace {
FILE *order_ui_log=nullptr;
bool order_ui_started=false;
uint order_ui_frames=0;
const OrderList *order_ui_target=nullptr;
std::string order_ui_expected;
bool order_ui_label_query=false;

[[noreturn]] void OrderUiFail(const char *reason)
{
 fprintf(order_ui_log,"FAIL %s\n",reason);fflush(order_ui_log);ExitProcess(202);
}
void OrderUiRequire(CommandCost result,const char *operation)
{
 if(result.Failed()){fprintf(order_ui_log,"COMMAND_FAILURE %s error=%s\n",operation,GetString(result.GetErrorMessage()).c_str());OrderUiFail("native fixture command failed");}
}
LONG WINAPI OrderUiExceptionFilter(EXCEPTION_POINTERS *exception)
{
 const auto *record=exception->ExceptionRecord;
 const auto *context=exception->ContextRecord;
 fprintf(order_ui_log,"EXCEPTION code=%08lX address=%p fault=%p rax=%p image=%p bytes=",record->ExceptionCode,record->ExceptionAddress,
  reinterpret_cast<void*>(record->NumberParameters>1?record->ExceptionInformation[1]:0),reinterpret_cast<void*>(context->Rax),GetModuleHandleW(nullptr));
 const auto *instruction=static_cast<const unsigned char*>(record->ExceptionAddress);
 for(uint i=0;i<16;i++)fprintf(order_ui_log,"%02X",instruction[i]);
 fprintf(order_ui_log,"\n");fflush(order_ui_log);ExitProcess(201);
 return EXCEPTION_EXECUTE_HANDLER;
}
std::string OrderUiContents(const OrderList *target)
{
 std::string value;
 for(const Order *order:target->Orders()){
  value+=std::to_string(uint(order->GetType()))+":"+std::to_string(order->GetDestination().base())+":"+std::to_string(order->GetRawFlags());
  if(order->IsType(OT_LABEL)&&order->GetLabelSubType()==OLST_TEXT)value+=std::string(":")+order->GetLabelText();
  value+=';';
 }
 return value;
}
Order OrderUiLabel(std::string_view text)
{
 Order order;order.MakeLabel(OLST_TEXT);order.SetLabelText(text);return order;
}
Point OrderUiPoint(Window *window,uint row)
{
 const auto rectangle=window->GetWidget<NWidgetBase>(WID_O_ORDER_LIST)->GetCurrentRect();
 const uint height=GetCharacterHeight(FontSize::Normal);
 return {rectangle.left+WidgetDimensions::scaled.framerect.left+5,
  rectangle.top+WidgetDimensions::scaled.framerect.top+int(row*height+height/2)};
}
void OrderUiClick(Window *window,Point point)
{
 _ctrl_pressed=false;_shift_pressed=false;
 _cursor.pos={window->left+point.x,window->top+point.y};
 window->OnClick(point,WID_O_ORDER_LIST,1);
 fprintf(order_ui_log,"AFTER_CLICK actual_callback=1 point=%d,%d\n",point.x,point.y);fflush(order_ui_log);
}
}

void OrderEditorDragDropTick()
{
 if(_game_mode!=GameMode::Normal||HasModalProgress()||_generating_world)return;
 if(!order_ui_log){order_ui_log=fopen(std::getenv("PXP_UI_LOG"),"w");SetUnhandledExceptionFilter(OrderUiExceptionFilter);}
 const std::string_view mode=std::getenv("PXP_UI_MODE");
 if(order_ui_started){
  /* Commands posted by GUI events may complete later in the ordinary loop. */
  if(OrderUiContents(order_ui_target)!=order_ui_expected){if(++order_ui_frames<16)return;OrderUiFail("actual callback produced incorrect order contents");}
  if(order_ui_label_query&&!HaveWindowByClass(WindowClass::QueryString))OrderUiFail("text-label click did not open native editor");
  fprintf(order_ui_log,"STRICT PASS order_editor=%.*s actual_click=1 actual_drop=%u orders=%u contents_unchanged_or_expected=1 native_label_query=%u\n",
   int(mode.size()),mode.data(),mode!="foreign-readonly",order_ui_target->GetNumOrders(),order_ui_label_query);fflush(order_ui_log);_exit_game=true;return;
 }
 order_ui_started=true;
 Window *window=nullptr;
 OrderListID list=OrderListID::Invalid();
 uint from_row=0,to_row=0;
 if(mode=="player-waypoint"){
  list=OrderListID{8116};const auto *target=OrderList::GetIfValid(list);
  if(!target||!target->IsPlayerCreated()||target->GetNumOrders()!=1||!target->GetOrderAt(0)->IsType(OT_GOTO_WAYPOINT))OrderUiFail("original crash schedule 8116 does not match evidence");
  SetLocalCompany(target->GetCompany());_current_company=target->GetCompany();
  fprintf(order_ui_log,"PLAYER_TARGET list=%u name=%s owner=%u orders=%u\n",list.base(),target->GetName().c_str(),target->GetCompany().base(),target->GetNumOrders());
 }else{
  const Company *local=nullptr;
  for(const auto *company:Company::Iterate()){local=company;break;}
  if(!local)OrderUiFail("stock fixture needs a real company");
  SetLocalCompany(local->index);_current_company=local->index;
  if(mode=="vehicle-click"||mode=="vehicle-label"){
   Train *vehicle=nullptr;
   for(Train *train:Train::Iterate())if(train->IsPrimaryVehicle()&&train->owner==_local_company&&train->GetNumOrders()>0){vehicle=train;break;}
   if(!vehicle)OrderUiFail("stock fixture needs a real train with orders");
   if(mode=="vehicle-label"){
    OrderUiRequire(CmdInsertOrder(DoCommandFlag::Execute,InsertOrderCmdData(vehicle->index,VehicleOrderID{0},OrderUiLabel("native vehicle label"))),"vehicle text label");
    OrderUiRequire(CmdModifyOrder(DoCommandFlag::Execute,OrderTargetType::Vehicle,vehicle->index.base(),VehicleOrderID{0},MOF_LABEL_TEXT,0,INVALID_CARGO,"native vehicle label"),"vehicle label text");
   }
   ShowOrdersWindow(vehicle);window=FindWindowById(WindowClass::VehicleOrders,vehicle->index.base());
   order_ui_label_query=mode=="vehicle-label";
  }else{
   CompanyID original=_local_company;
   if(mode=="foreign-readonly"){
    Company *other=DoStartupNewCompany(DSNC_NONE);if(!other)OrderUiFail("cannot create foreign owner fixture");_current_company=other->index;
   }
   auto result=CmdCreateOrderList(DoCommandFlag::Execute,"native UI drag/drop regression",OrderListID::Invalid(),false);
   OrderUiRequire(result,"standalone schedule");list=*result.GetResultData<OrderListID>();
   const auto append=[&](const Order &order){
    const auto position=OrderList::Get(list)->GetNumOrders();
    OrderUiRequire(CmdInsertOrder(DoCommandFlag::Execute,InsertOrderCmdData(list,position,order)),"standalone order");
    if(order.IsType(OT_LABEL)&&order.GetLabelSubType()==OLST_TEXT)OrderUiRequire(CmdModifyOrder(DoCommandFlag::Execute,OrderTargetType::OrderList,list.base(),position,MOF_LABEL_TEXT,0,INVALID_CARGO,order.GetLabelText()),"standalone label text");
   };
   if(mode=="waypoint-click"||mode=="end-marker"){
    const TileIndex tile=TileXY(130,40);OrderUiRequire(CmdBuildRailWaypoint(DoCommandFlag::Execute,tile,Axis::X,1,1,STAT_CLASS_WAYP,0,StationID::Invalid(),false),"stock waypoint");
    Order order;order.MakeGoToWaypoint(GetStationIndex(tile));append(order);
    if(mode=="end-marker")from_row=to_row=1;
   }else if(mode=="move-distinct"){
    append(OrderUiLabel("A"));append(OrderUiLabel("B"));append(OrderUiLabel("C"));to_row=2;
   }else if(mode=="delete-selection"){
    append(OrderUiLabel("A"));append(OrderUiLabel("B"));
   }else if(mode!="empty-end"){
    append(OrderUiLabel("native standalone label"));order_ui_label_query=mode=="label-click";
   }
   if(mode=="foreign-readonly"){
    OrderUiRequire(CmdSetOrderListPublic(DoCommandFlag::Execute,list,true),"foreign public visibility");
    /* Native commands above ran as the foreign owner; leave that context
     * before invoking a local-company GUI event. */
    _current_company=original;
    if(_local_company!=original)OrderUiFail("foreign setup changed the local client");
   }
  }
 }
 if(!window){ShowOrderListEditor(list);window=FindWindowById(WindowClass::OrderListEditor,list.base());}
 auto *general=dynamic_cast<GeneralVehicleWindow*>(window);
 if(!window||!general||!general->GetTargetOrders())OrderUiFail("native editor factory failed");
 order_ui_target=general->GetTargetOrders();order_ui_expected=OrderUiContents(order_ui_target);
 fprintf(order_ui_log,"ACTUAL_WINDOW class=%u list=%u vehicle=%p orders=%u contents=%s\n",uint(window->window_class),list.base(),general->vehicle,order_ui_target->GetNumOrders(),order_ui_expected.c_str());fflush(order_ui_log);
 const Point from=OrderUiPoint(window,from_row),to=OrderUiPoint(window,to_row);
 OrderUiClick(window,from);
 if(mode=="delete-selection"){
  OrderUiRequire(CmdDeleteOrder(DoCommandFlag::Execute,OrderTargetType::OrderList,list.base(),VehicleOrderID{0}),"delete selected row natively");
  order_ui_expected=OrderUiContents(order_ui_target);
 }else if(mode=="move-distinct"){
  const auto *target=order_ui_target;
  order_ui_expected=std::to_string(uint(OT_LABEL))+":"+std::to_string(target->GetOrderAt(1)->GetDestination().base())+":"+std::to_string(target->GetOrderAt(1)->GetRawFlags())+":B;"+
   std::to_string(uint(OT_LABEL))+":"+std::to_string(target->GetOrderAt(2)->GetDestination().base())+":"+std::to_string(target->GetOrderAt(2)->GetRawFlags())+":C;"+
   std::to_string(uint(OT_LABEL))+":"+std::to_string(target->GetOrderAt(0)->GetDestination().base())+":"+std::to_string(target->GetOrderAt(0)->GetRawFlags())+":A;";
 }
 if(mode!="foreign-readonly"){
  window->OnDragDrop(to,WID_O_ORDER_LIST);
  fprintf(order_ui_log,"AFTER_DROP actual_callback=1 from=%u to=%u\n",from_row,to_row);fflush(order_ui_log);
 }else{
  /* Read-only OnClick never starts a drag, so no artificial drop is sent. */
  if(general->order_list->GetCompany()==_local_company)OrderUiFail("foreign fixture unexpectedly locally owned");
 }
}
