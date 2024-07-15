/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
// vim:set ts=2 sts=2 sw=2 et cin:
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifdef MOZ_X11
#include <cairo-xlib.h>
#include "gfxXlibSurface.h"
/* X headers suck */
enum { XKeyPress = KeyPress };
#include "mozilla/X11Util.h"
using mozilla::DefaultXDisplay;
#endif

#include "nsPluginInstanceOwner.h"

#include "gfxUtils.h"
#include "nsIRunnable.h"
#include "nsContentUtils.h"
#include "nsRect.h"
#include "nsSize.h"
#include "nsDisplayList.h"
#include "ImageLayers.h"
#include "GLImages.h"
#include "nsPluginFrame.h"
#include "nsIPluginDocument.h"
#include "nsIStringStream.h"
#include "nsNetUtil.h"
#include "mozilla/Preferences.h"
#include "nsILinkHandler.h"
#include "nsIDocShellTreeItem.h"
#include "nsIWebBrowserChrome.h"
#include "nsLayoutUtils.h"
#include "nsIPluginWidget.h"
#include "nsViewManager.h"
#include "nsIDocShellTreeOwner.h"
#include "nsIDOMHTMLObjectElement.h"
#include "nsIAppShell.h"
#include "nsIObjectLoadingContent.h"
#include "nsObjectLoadingContent.h"
#include "nsAttrName.h"
#include "nsIFocusManager.h"
#include "nsFocusManager.h"
#include "nsIDOMDragEvent.h"
#include "nsIScriptSecurityManager.h"
#include "nsIScrollableFrame.h"
#include "nsIDocShell.h"
#include "ImageContainer.h"
#include "nsIDOMHTMLCollection.h"
#include "GLContext.h"
#include "EGLUtils.h"
#include "nsIContentInlines.h"
#include "mozilla/MiscEvents.h"
#include "mozilla/MouseEvents.h"
#include "mozilla/TextEvents.h"
#include "mozilla/dom/Event.h"
#include "mozilla/dom/HTMLObjectElementBinding.h"
#include "mozilla/dom/TabChild.h"
#include "nsFrameSelection.h"
#include "PuppetWidget.h"
#include "nsPIWindowRoot.h"
#include "mozilla/IMEStateManager.h"
#include "mozilla/TextComposition.h"
#include "mozilla/AutoRestore.h"

#include "nsContentCID.h"
#include "nsWidgetsCID.h"
static NS_DEFINE_CID(kWidgetCID, NS_CHILD_CID);
static NS_DEFINE_CID(kAppShellCID, NS_APPSHELL_CID);

#ifdef MOZ_WIDGET_GTK
#include <gdk/gdk.h>
#include <gtk/gtk.h>
#endif

using namespace mozilla;
using namespace mozilla::dom;
using namespace mozilla::layers;

static inline nsPoint AsNsPoint(const nsIntPoint &p) {
  return nsPoint(p.x, p.y);
}

// special class for handeling DOM context menu events because for
// some reason it starves other mouse events if implemented on the
// same class
class nsPluginDOMContextMenuListener : public nsIDOMEventListener
{
  virtual ~nsPluginDOMContextMenuListener();

public:
  explicit nsPluginDOMContextMenuListener(nsIContent* aContent);

  NS_DECL_ISUPPORTS
  NS_DECL_NSIDOMEVENTLISTENER

  void Destroy(nsIContent* aContent);

  nsEventStatus ProcessEvent(const WidgetGUIEvent& anEvent)
  {
    return nsEventStatus_eConsumeNoDefault;
  }
};

class AsyncPaintWaitEvent : public Runnable
{
public:
  AsyncPaintWaitEvent(nsIContent* aContent, bool aFinished) :
    Runnable("AsyncPaintWaitEvent"),
    mContent(aContent),
    mFinished(aFinished)
  {
  }

  NS_IMETHOD Run() override
  {
    nsContentUtils::DispatchTrustedEvent(mContent->OwnerDoc(), mContent,
        mFinished ? NS_LITERAL_STRING("MozPaintWaitFinished") : NS_LITERAL_STRING("MozPaintWait"),
        true, true);
    return NS_OK;
  }

private:
  nsCOMPtr<nsIContent> mContent;
  bool                 mFinished;
};

void
nsPluginInstanceOwner::NotifyPaintWaiter(nsDisplayListBuilder* aBuilder)
{
  if (!mWaitingForPaint && !IsUpToDate() && aBuilder->ShouldSyncDecodeImages()) {
    nsCOMPtr<nsIContent> content = do_QueryReferent(mContent);
    nsCOMPtr<nsIRunnable> event = new AsyncPaintWaitEvent(content, false);
    // Run this event as soon as it's safe to do so, since listeners need to
    // receive it immediately
    nsContentUtils::AddScriptRunner(event);
    mWaitingForPaint = true;
  }
}

bool
nsPluginInstanceOwner::NeedsScrollImageLayer()
{
  return false;
}

already_AddRefed<ImageContainer>
nsPluginInstanceOwner::GetImageContainer()
{
  if (!mInstance)
    return nullptr;

  RefPtr<ImageContainer> container;

  if (NeedsScrollImageLayer()) {
    // windowed plugin under e10s
  } else {
    // async windowless rendering
    mInstance->GetImageContainer(getter_AddRefs(container));
  }

  return container.forget();
}

void
nsPluginInstanceOwner::DidComposite()
{
  if (mInstance) {
    mInstance->DidComposite();
  }
}

void
nsPluginInstanceOwner::SetBackgroundUnknown()
{
  if (mInstance) {
    mInstance->SetBackgroundUnknown();
  }
}

already_AddRefed<mozilla::gfx::DrawTarget>
nsPluginInstanceOwner::BeginUpdateBackground(const nsIntRect& aRect)
{
  nsIntRect rect = aRect;
  RefPtr<DrawTarget> dt;
  if (mInstance &&
      NS_SUCCEEDED(mInstance->BeginUpdateBackground(&rect, getter_AddRefs(dt)))) {
    return dt.forget();
  }
  return nullptr;
}

void
nsPluginInstanceOwner::EndUpdateBackground(const nsIntRect& aRect)
{
  nsIntRect rect = aRect;
  if (mInstance) {
    mInstance->EndUpdateBackground(&rect);
  }
}

bool
nsPluginInstanceOwner::UseAsyncRendering()
{
  bool isOOP;
  bool result = (mInstance &&
          NS_SUCCEEDED(mInstance->GetIsOOP(&isOOP)) && isOOP
          && (!mPluginWindow ||
           mPluginWindow->type == NPWindowTypeDrawable)
          );

  return result;
}

nsIntSize
nsPluginInstanceOwner::GetCurrentImageSize()
{
  nsIntSize size(0,0);
  if (mInstance) {
    mInstance->GetImageSize(&size);
  }
  return size;
}

nsPluginInstanceOwner::nsPluginInstanceOwner()
  : mPluginWindow(nullptr)
{
  // create nsPluginNativeWindow object, it is derived from NPWindow
  // struct and allows to manipulate native window procedure
  nsCOMPtr<nsIPluginHost> pluginHostCOM = do_GetService(MOZ_PLUGIN_HOST_CONTRACTID);
  mPluginHost = static_cast<nsPluginHost*>(pluginHostCOM.get());
  if (mPluginHost)
    mPluginHost->NewPluginNativeWindow(&mPluginWindow);

  mPluginFrame = nullptr;
  mWidgetCreationComplete = false;
  mLastCSSZoomFactor = 1.0;
  mContentFocused = false;
  mWidgetVisible = true;
  mPluginWindowVisible = false;
  mPluginDocumentActiveState = true;
  mLastMouseDownButtonType = -1;

  mWaitingForPaint = false;
}

nsPluginInstanceOwner::~nsPluginInstanceOwner()
{
  if (mWaitingForPaint) {
    nsCOMPtr<nsIContent> content = do_QueryReferent(mContent);
    if (content) {
      // We don't care when the event is dispatched as long as it's "soon",
      // since whoever needs it will be waiting for it.
      nsCOMPtr<nsIRunnable> event = new AsyncPaintWaitEvent(content, true);
      NS_DispatchToMainThread(event);
    }
  }

  mPluginFrame = nullptr;

  PLUG_DeletePluginNativeWindow(mPluginWindow);
  mPluginWindow = nullptr;

  if (mInstance) {
    mInstance->SetOwner(nullptr);
  }
}

NS_IMPL_ISUPPORTS(nsPluginInstanceOwner,
                  nsIPluginInstanceOwner,
                  nsIDOMEventListener,
                  nsIPrivacyTransitionObserver,
                  nsIKeyEventInPluginCallback,
                  nsISupportsWeakReference)

nsresult
nsPluginInstanceOwner::SetInstance(nsNPAPIPluginInstance *aInstance)
{
  NS_ASSERTION(!mInstance || !aInstance, "mInstance should only be set or unset!");

  // If we're going to null out mInstance after use, be sure to call
  // mInstance->SetOwner(nullptr) here, since it now won't be called
  // from our destructor.  This fixes bug 613376.
  if (mInstance && !aInstance) {
    mInstance->SetOwner(nullptr);
  }

  mInstance = aInstance;

  nsCOMPtr<nsIDocument> doc;
  GetDocument(getter_AddRefs(doc));
  if (doc) {
    if (nsCOMPtr<nsPIDOMWindowOuter> domWindow = doc->GetWindow()) {
      nsCOMPtr<nsIDocShell> docShell = domWindow->GetDocShell();
      if (docShell)
        docShell->AddWeakPrivacyTransitionObserver(this);
    }
  }

  return NS_OK;
}

NS_IMETHODIMP nsPluginInstanceOwner::GetWindow(NPWindow *&aWindow)
{
  NS_ASSERTION(mPluginWindow, "the plugin window object being returned is null");
  aWindow = mPluginWindow;
  return NS_OK;
}

NS_IMETHODIMP nsPluginInstanceOwner::GetMode(int32_t *aMode)
{
  nsCOMPtr<nsIDocument> doc;
  nsresult rv = GetDocument(getter_AddRefs(doc));
  nsCOMPtr<nsIPluginDocument> pDoc (do_QueryInterface(doc));

  if (pDoc) {
    *aMode = NP_FULL;
  } else {
    *aMode = NP_EMBED;
  }

  return rv;
}

void nsPluginInstanceOwner::GetAttributes(nsTArray<MozPluginParameter>& attributes)
{
  nsCOMPtr<nsIObjectLoadingContent> content = do_QueryReferent(mContent);
  nsObjectLoadingContent *loadingContent =
    static_cast<nsObjectLoadingContent*>(content.get());

  loadingContent->GetPluginAttributes(attributes);
}

NS_IMETHODIMP nsPluginInstanceOwner::GetDOMElement(nsIDOMElement* *result)
{
  return CallQueryReferent(mContent.get(), result);
}

nsresult nsPluginInstanceOwner::GetInstance(nsNPAPIPluginInstance **aInstance)
{
  NS_ENSURE_ARG_POINTER(aInstance);

  *aInstance = mInstance;
  NS_IF_ADDREF(*aInstance);
  return NS_OK;
}

NS_IMETHODIMP nsPluginInstanceOwner::GetURL(const char *aURL,
                                            const char *aTarget,
                                            nsIInputStream *aPostStream,
                                            void *aHeadersData,
                                            uint32_t aHeadersDataLen,
                                            bool aDoCheckLoadURIChecks)
{
  nsCOMPtr<nsIContent> content = do_QueryReferent(mContent);
  if (!content) {
    return NS_ERROR_NULL_POINTER;
  }

  if (content->IsEditable()) {
    return NS_OK;
  }

  nsIDocument *doc = content->GetUncomposedDoc();
  if (!doc) {
    return NS_ERROR_FAILURE;
  }

  nsIPresShell *presShell = doc->GetShell();
  if (!presShell) {
    return NS_ERROR_FAILURE;
  }

  nsPresContext *presContext = presShell->GetPresContext();
  if (!presContext) {
    return NS_ERROR_FAILURE;
  }

  // the container of the pres context will give us the link handler
  nsCOMPtr<nsISupports> container = presContext->GetContainerWeak();
  NS_ENSURE_TRUE(container,NS_ERROR_FAILURE);
  nsCOMPtr<nsILinkHandler> lh = do_QueryInterface(container);
  NS_ENSURE_TRUE(lh, NS_ERROR_FAILURE);

  nsAutoString unitarget;
  if ((0 == PL_strcmp(aTarget, "newwindow")) ||
      (0 == PL_strcmp(aTarget, "_new"))) {
    unitarget.AssignASCII("_blank");
  }
  else if (0 == PL_strcmp(aTarget, "_current")) {
    unitarget.AssignASCII("_self");
  }
  else {
    unitarget.AssignASCII(aTarget); // XXX could this be nonascii?
  }

  nsCOMPtr<nsIURI> baseURI = GetBaseURI();

  // Create an absolute URL
  nsCOMPtr<nsIURI> uri;
  nsresult rv = NS_NewURI(getter_AddRefs(uri), aURL, baseURI);
  NS_ENSURE_SUCCESS(rv, NS_ERROR_FAILURE);

  nsCOMPtr<nsIInputStream> headersDataStream;
  if (aPostStream && aHeadersData) {
    if (!aHeadersDataLen)
      return NS_ERROR_UNEXPECTED;

    nsCOMPtr<nsIStringInputStream> sis = do_CreateInstance("@mozilla.org/io/string-input-stream;1");
    if (!sis)
      return NS_ERROR_OUT_OF_MEMORY;

    rv = sis->SetData((char *)aHeadersData, aHeadersDataLen);
    NS_ENSURE_SUCCESS(rv, rv);
    headersDataStream = do_QueryInterface(sis);
  }

  int32_t blockPopups =
    Preferences::GetInt("privacy.popups.disable_from_plugins");
  nsAutoPopupStatePusher popupStatePusher((PopupControlState)blockPopups);


  // if security checks (in particular CheckLoadURIWithPrincipal) needs
  // to be skipped we are creating a codebasePrincipal to make sure
  // that security check succeeds. Please note that we do not want to
  // fall back to using the systemPrincipal, because that would also
  // bypass ContentPolicy checks which should still be enforced.
  nsCOMPtr<nsIPrincipal> triggeringPrincipal;
  if (!aDoCheckLoadURIChecks) {
    mozilla::OriginAttributes attrs =
      BasePrincipal::Cast(content->NodePrincipal())->OriginAttributesRef();
    triggeringPrincipal = BasePrincipal::CreateCodebasePrincipal(uri, attrs);
  }

  rv = lh->OnLinkClick(content, uri, unitarget.get(), VoidString(),
                       aPostStream, headersDataStream, true, triggeringPrincipal);

  return rv;
}

NS_IMETHODIMP nsPluginInstanceOwner::GetDocument(nsIDocument* *aDocument)
{
  nsCOMPtr<nsIContent> content = do_QueryReferent(mContent);
  if (!aDocument || !content) {
    return NS_ERROR_NULL_POINTER;
  }

  // XXX sXBL/XBL2 issue: current doc or owner doc?
  // But keep in mind bug 322414 comment 33
  NS_IF_ADDREF(*aDocument = content->OwnerDoc());
  return NS_OK;
}

NS_IMETHODIMP nsPluginInstanceOwner::InvalidateRect(NPRect *invalidRect)
{
  // If our object frame has gone away, we won't be able to determine
  // up-to-date-ness, so just fire off the event.
  if (mWaitingForPaint && (!mPluginFrame || IsUpToDate())) {
    nsCOMPtr<nsIContent> content = do_QueryReferent(mContent);
    // We don't care when the event is dispatched as long as it's "soon",
    // since whoever needs it will be waiting for it.
    nsCOMPtr<nsIRunnable> event = new AsyncPaintWaitEvent(content, true);
    NS_DispatchToMainThread(event);
    mWaitingForPaint = false;
  }

  if (!mPluginFrame || !invalidRect || !mWidgetVisible)
    return NS_ERROR_FAILURE;

  // Invalidate for windowed plugins needs to work.
  if (mWidget) {
    mWidget->Invalidate(
      LayoutDeviceIntRect(invalidRect->left, invalidRect->top,
                          invalidRect->right - invalidRect->left,
                          invalidRect->bottom - invalidRect->top));
    // Plugin instances also call invalidate when plugin windows are hidden
    // during scrolling. In this case fall through so we invalidate the
    // underlying layer.
    if (!NeedsScrollImageLayer()) {
      return NS_OK;
    }
  }
  nsIntRect rect(invalidRect->left,
                 invalidRect->top,
                 invalidRect->right - invalidRect->left,
                 invalidRect->bottom - invalidRect->top);
  // invalidRect is in "display pixels".  In non-HiDPI modes "display pixels"
  // are device pixels.  But in HiDPI modes each display pixel corresponds
  // to more than one device pixel.
  double scaleFactor = 1.0;
  GetContentsScaleFactor(&scaleFactor);
  rect.ScaleRoundOut(scaleFactor);
  mPluginFrame->InvalidateLayer(nsDisplayItem::TYPE_PLUGIN, &rect);
  return NS_OK;
}

NS_IMETHODIMP nsPluginInstanceOwner::InvalidateRegion(NPRegion invalidRegion)
{
  return NS_ERROR_NOT_IMPLEMENTED;
}

NS_IMETHODIMP
nsPluginInstanceOwner::RedrawPlugin()
{
  if (mPluginFrame) {
    mPluginFrame->InvalidateLayer(nsDisplayItem::TYPE_PLUGIN);
  }
  return NS_OK;
}

NS_IMETHODIMP nsPluginInstanceOwner::GetNetscapeWindow(void *value)
{
  if (!mPluginFrame) {
    NS_WARNING("plugin owner has no owner in getting doc's window handle");
    return NS_ERROR_FAILURE;
  }

#if defined(MOZ_WIDGET_GTK) && defined(MOZ_X11)
  // X11 window managers want the toplevel window for WM_TRANSIENT_FOR.
  nsIWidget* win = mPluginFrame->GetNearestWidget();
  if (!win)
    return NS_ERROR_FAILURE;
  *static_cast<Window*>(value) = (long unsigned int)win->GetNativeData(NS_NATIVE_SHAREABLE_WINDOW);
  return NS_OK;
#else
  return NS_ERROR_NOT_IMPLEMENTED;
#endif
}

void
nsPluginInstanceOwner::HandledWindowedPluginKeyEvent(
                         const NativeEventData& aKeyEventData,
                         bool aIsConsumed)
{
  if (NS_WARN_IF(!mInstance)) {
    return;
  }
  DebugOnly<nsresult> rv =
    mInstance->HandledWindowedPluginKeyEvent(aKeyEventData, aIsConsumed);
  NS_WARNING_ASSERTION(NS_SUCCEEDED(rv), "HandledWindowedPluginKeyEvent fail");
}

void
nsPluginInstanceOwner::OnWindowedPluginKeyEvent(
                         const NativeEventData& aKeyEventData)
{
  if (NS_WARN_IF(!mPluginFrame)) {
    // Notifies the plugin process of the key event being not consumed by us.
    HandledWindowedPluginKeyEvent(aKeyEventData, false);
    return;
  }

  nsCOMPtr<nsIWidget> widget = mPluginFrame->PresContext()->GetRootWidget();
  if (NS_WARN_IF(!widget)) {
    // Notifies the plugin process of the key event being not consumed by us.
    HandledWindowedPluginKeyEvent(aKeyEventData, false);
    return;
  }

  nsresult rv = widget->OnWindowedPluginKeyEvent(aKeyEventData, this);
  if (NS_WARN_IF(NS_FAILED(rv))) {
    // Notifies the plugin process of the key event being not consumed by us.
    HandledWindowedPluginKeyEvent(aKeyEventData, false);
    return;
  }

  // If the key event is posted to another process, we need to wait a call
  // of HandledWindowedPluginKeyEvent().  So, nothing to do here in this case.
  if (rv == NS_SUCCESS_EVENT_HANDLED_ASYNCHRONOUSLY) {
    return;
  }

  // Otherwise, the key event is handled synchronously.  Let's notify the
  // plugin process of the key event's result.
  bool consumed = (rv == NS_SUCCESS_EVENT_CONSUMED);
  HandledWindowedPluginKeyEvent(aKeyEventData, consumed);
}

NS_IMETHODIMP nsPluginInstanceOwner::SetEventModel(int32_t eventModel)
{
  return NS_ERROR_NOT_IMPLEMENTED;
}

NPBool nsPluginInstanceOwner::ConvertPoint(double sourceX, double sourceY, NPCoordinateSpace sourceSpace,
                                           double *destX, double *destY, NPCoordinateSpace destSpace)
{
  return false;
}

NPError nsPluginInstanceOwner::InitAsyncSurface(NPSize *size, NPImageFormat format,
                                                void *initData, NPAsyncSurface *surface)
{
  return NPERR_INCOMPATIBLE_VERSION_ERROR;
}

NPError nsPluginInstanceOwner::FinalizeAsyncSurface(NPAsyncSurface *)
{
  return NPERR_INCOMPATIBLE_VERSION_ERROR;
}

void nsPluginInstanceOwner::SetCurrentAsyncSurface(NPAsyncSurface *, NPRect*)
{
}

NS_IMETHODIMP nsPluginInstanceOwner::GetTagType(nsPluginTagType *result)
{
  NS_ENSURE_ARG_POINTER(result);

  *result = nsPluginTagType_Unknown;

  nsCOMPtr<nsIContent> content = do_QueryReferent(mContent);
  if (content->IsHTMLElement(nsGkAtoms::embed))
    *result = nsPluginTagType_Embed;
  else if (content->IsHTMLElement(nsGkAtoms::object))
    *result = nsPluginTagType_Object;

  return NS_OK;
}

void nsPluginInstanceOwner::GetParameters(nsTArray<MozPluginParameter>& parameters)
{
  nsCOMPtr<nsIObjectLoadingContent> content = do_QueryReferent(mContent);
  nsObjectLoadingContent *loadingContent =
    static_cast<nsObjectLoadingContent*>(content.get());

  loadingContent->GetPluginParameters(parameters);
}


// static
uint32_t
nsPluginInstanceOwner::GetEventloopNestingLevel()
{
  nsCOMPtr<nsIAppShell> appShell = do_GetService(kAppShellCID);
  uint32_t currentLevel = 0;
  if (appShell) {
    appShell->GetEventloopNestingLevel(&currentLevel);
  }

  // No idea how this happens... but Linux doesn't consistently
  // process UI events through the appshell event loop. If we get a 0
  // here on any platform we increment the level just in case so that
  // we make sure we always tear the plugin down eventually.
  if (!currentLevel) {
    currentLevel++;
  }

  return currentLevel;
}

nsresult nsPluginInstanceOwner::DispatchFocusToPlugin(nsIDOMEvent* aFocusEvent)
{
  if (!mPluginWindow || (mPluginWindow->type == NPWindowTypeWindow)) {
    // continue only for cases without child window
    return aFocusEvent->PreventDefault(); // consume event
  }

  WidgetEvent* theEvent = aFocusEvent->WidgetEventPtr();
  if (theEvent) {
    WidgetGUIEvent focusEvent(theEvent->IsTrusted(), theEvent->mMessage,
                              nullptr);
    nsEventStatus rv = ProcessEvent(focusEvent);
    if (nsEventStatus_eConsumeNoDefault == rv) {
      aFocusEvent->PreventDefault();
      aFocusEvent->StopPropagation();
    }
  }

  return NS_OK;
}

nsresult nsPluginInstanceOwner::ProcessKeyPress(nsIDOMEvent* aKeyEvent)
{
  if (SendNativeEvents())
    DispatchKeyToPlugin(aKeyEvent);

  if (mInstance) {
    // If this event is going to the plugin, we want to kill it.
    // Not actually sending keypress to the plugin, since we didn't before.
    aKeyEvent->PreventDefault();
    aKeyEvent->StopPropagation();
  }
  return NS_OK;
}

nsresult nsPluginInstanceOwner::DispatchKeyToPlugin(nsIDOMEvent* aKeyEvent)
{
  if (!mPluginWindow || (mPluginWindow->type == NPWindowTypeWindow))
    return aKeyEvent->PreventDefault(); // consume event
  // continue only for cases without child window

  if (mInstance) {
    WidgetKeyboardEvent* keyEvent =
      aKeyEvent->WidgetEventPtr()->AsKeyboardEvent();
    if (keyEvent && keyEvent->mClass == eKeyboardEventClass) {
      nsEventStatus rv = ProcessEvent(*keyEvent);
      if (nsEventStatus_eConsumeNoDefault == rv) {
        aKeyEvent->PreventDefault();
        aKeyEvent->StopPropagation();
      }
    }
  }

  return NS_OK;
}

nsresult
nsPluginInstanceOwner::ProcessMouseDown(nsIDOMEvent* aMouseEvent)
{
  if (!mPluginWindow || (mPluginWindow->type == NPWindowTypeWindow))
    return aMouseEvent->PreventDefault(); // consume event
  // continue only for cases without child window

  // if the plugin is windowless, we need to set focus ourselves
  // otherwise, we might not get key events
  if (mPluginFrame && mPluginWindow &&
      mPluginWindow->type == NPWindowTypeDrawable) {

    nsIFocusManager* fm = nsFocusManager::GetFocusManager();
    if (fm) {
      nsCOMPtr<nsIDOMElement> elem = do_QueryReferent(mContent);
      fm->SetFocus(elem, 0);
    }
  }

  WidgetMouseEvent* mouseEvent =
    aMouseEvent->WidgetEventPtr()->AsMouseEvent();
  if (mouseEvent && mouseEvent->mClass == eMouseEventClass) {
    mLastMouseDownButtonType = mouseEvent->button;
    nsEventStatus rv = ProcessEvent(*mouseEvent);
    if (nsEventStatus_eConsumeNoDefault == rv) {
      return aMouseEvent->PreventDefault(); // consume event
    }
  }

  return NS_OK;
}

nsresult nsPluginInstanceOwner::DispatchMouseToPlugin(nsIDOMEvent* aMouseEvent,
                                                      bool aAllowPropagate)
{
  if (!mPluginWindow || (mPluginWindow->type == NPWindowTypeWindow))
    return aMouseEvent->PreventDefault(); // consume event
  // continue only for cases without child window
  // don't send mouse events if we are hidden
  if (!mWidgetVisible)
    return NS_OK;

  WidgetMouseEvent* mouseEvent =
    aMouseEvent->WidgetEventPtr()->AsMouseEvent();
  if (mouseEvent && mouseEvent->mClass == eMouseEventClass) {
    nsEventStatus rv = ProcessEvent(*mouseEvent);
    if (nsEventStatus_eConsumeNoDefault == rv) {
      aMouseEvent->PreventDefault();
      if (!aAllowPropagate) {
        aMouseEvent->StopPropagation();
      }
    }
    if (mouseEvent->mMessage == eMouseUp) {
      mLastMouseDownButtonType = -1;
    }
  }
  return NS_OK;
}

nsresult
nsPluginInstanceOwner::DispatchCompositionToPlugin(nsIDOMEvent* aEvent)
{
  return NS_OK;
}

nsresult
nsPluginInstanceOwner::HandleEvent(nsIDOMEvent* aEvent)
{
  NS_ASSERTION(mInstance, "Should have a valid plugin instance or not receive events.");

  nsAutoString eventType;
  aEvent->GetType(eventType);

  if (eventType.EqualsLiteral("focus")) {
    mContentFocused = true;
    return DispatchFocusToPlugin(aEvent);
  }
  if (eventType.EqualsLiteral("blur")) {
    mContentFocused = false;
    return DispatchFocusToPlugin(aEvent);
  }
  if (eventType.EqualsLiteral("mousedown")) {
    return ProcessMouseDown(aEvent);
  }
  if (eventType.EqualsLiteral("mouseup")) {
    return DispatchMouseToPlugin(aEvent);
  }
  if (eventType.EqualsLiteral("mousemove")) {
    return DispatchMouseToPlugin(aEvent, true);
  }
  if (eventType.EqualsLiteral("click") ||
      eventType.EqualsLiteral("dblclick") ||
      eventType.EqualsLiteral("mouseover") ||
      eventType.EqualsLiteral("mouseout")) {
    return DispatchMouseToPlugin(aEvent);
  }
  if (eventType.EqualsLiteral("keydown") ||
      eventType.EqualsLiteral("keyup")) {
    return DispatchKeyToPlugin(aEvent);
  }
  if (eventType.EqualsLiteral("keypress")) {
    return ProcessKeyPress(aEvent);
  }
  if (eventType.EqualsLiteral("compositionstart") ||
      eventType.EqualsLiteral("compositionend") ||
      eventType.EqualsLiteral("text")) {
    return DispatchCompositionToPlugin(aEvent);
  }

  nsCOMPtr<nsIDOMDragEvent> dragEvent(do_QueryInterface(aEvent));
  if (dragEvent && mInstance) {
    WidgetEvent* ievent = aEvent->WidgetEventPtr();
    if (ievent && ievent->IsTrusted() &&
        ievent->mMessage != eDragEnter && ievent->mMessage != eDragOver) {
      aEvent->PreventDefault();
    }

    // Let the plugin handle drag events.
    aEvent->StopPropagation();
  }
  return NS_OK;
}

#ifdef MOZ_X11
static unsigned int XInputEventState(const WidgetInputEvent& anEvent)
{
  unsigned int state = 0;
  if (anEvent.IsShift()) state |= ShiftMask;
  if (anEvent.IsControl()) state |= ControlMask;
  if (anEvent.IsAlt()) state |= Mod1Mask;
  if (anEvent.IsMeta()) state |= Mod4Mask;
  return state;
}
#endif

nsEventStatus nsPluginInstanceOwner::ProcessEvent(const WidgetGUIEvent& anEvent)
{
  nsEventStatus rv = nsEventStatus_eIgnore;

  if (!mInstance || !mPluginFrame) {
    return nsEventStatus_eIgnore;
  }

#ifdef MOZ_X11
  // this code supports windowless plugins
  nsIWidget* widget = anEvent.mWidget;
  XEvent pluginEvent = XEvent();
  pluginEvent.type = 0;

  switch(anEvent.mClass) {
    case eMouseEventClass:
      {
        switch (anEvent.mMessage) {
          case eMouseClick:
          case eMouseDoubleClick:
          case eMouseAuxClick:
            // Button up/down events sent instead.
            return rv;
          default:
            break;
          }

        // Get reference point relative to plugin origin.
        const nsPresContext* presContext = mPluginFrame->PresContext();
        nsPoint appPoint =
          nsLayoutUtils::GetEventCoordinatesRelativeTo(&anEvent, mPluginFrame) -
          mPluginFrame->GetContentRectRelativeToSelf().TopLeft();
        nsIntPoint pluginPoint(presContext->AppUnitsToDevPixels(appPoint.x),
                               presContext->AppUnitsToDevPixels(appPoint.y));
        const WidgetMouseEvent& mouseEvent = *anEvent.AsMouseEvent();
        // Get reference point relative to screen:
        LayoutDeviceIntPoint rootPoint(-1, -1);
        if (widget) {
          rootPoint = anEvent.mRefPoint + widget->WidgetToScreenOffset();
        }
#ifdef MOZ_WIDGET_GTK
        Window root = GDK_ROOT_WINDOW();
#else
        Window root = X11None; // Could XQueryTree, but this is not important.
#endif

        switch (anEvent.mMessage) {
          case eMouseOver:
          case eMouseOut:
            {
              XCrossingEvent& event = pluginEvent.xcrossing;
              event.type = anEvent.mMessage == eMouseOver ?
                EnterNotify : LeaveNotify;
              event.root = root;
              event.time = anEvent.mTime;
              event.x = pluginPoint.x;
              event.y = pluginPoint.y;
              event.x_root = rootPoint.x;
              event.y_root = rootPoint.y;
              event.state = XInputEventState(mouseEvent);
              // information lost
              event.subwindow = X11None;
              event.mode = -1;
              event.detail = NotifyDetailNone;
              event.same_screen = True;
              event.focus = mContentFocused;
            }
            break;
          case eMouseMove:
            {
              XMotionEvent& event = pluginEvent.xmotion;
              event.type = MotionNotify;
              event.root = root;
              event.time = anEvent.mTime;
              event.x = pluginPoint.x;
              event.y = pluginPoint.y;
              event.x_root = rootPoint.x;
              event.y_root = rootPoint.y;
              event.state = XInputEventState(mouseEvent);
              // information lost
              event.subwindow = X11None;
              event.is_hint = NotifyNormal;
              event.same_screen = True;
            }
            break;
          case eMouseDown:
          case eMouseUp:
            {
              XButtonEvent& event = pluginEvent.xbutton;
              event.type = anEvent.mMessage == eMouseDown ?
                ButtonPress : ButtonRelease;
              event.root = root;
              event.time = anEvent.mTime;
              event.x = pluginPoint.x;
              event.y = pluginPoint.y;
              event.x_root = rootPoint.x;
              event.y_root = rootPoint.y;
              event.state = XInputEventState(mouseEvent);
              switch (mouseEvent.button)
                {
                case WidgetMouseEvent::eMiddleButton:
                  event.button = 2;
                  break;
                case WidgetMouseEvent::eRightButton:
                  event.button = 3;
                  break;
                default: // WidgetMouseEvent::eLeftButton;
                  event.button = 1;
                  break;
                }
              // information lost:
              event.subwindow = X11None;
              event.same_screen = True;
            }
            break;
          default:
            break;
          }
      }
      break;

   //XXX case eMouseScrollEventClass: not received.

   case eKeyboardEventClass:
      if (anEvent.mPluginEvent)
        {
          XKeyEvent &event = pluginEvent.xkey;
#ifdef MOZ_WIDGET_GTK
          event.root = GDK_ROOT_WINDOW();
          event.time = anEvent.mTime;
          const GdkEventKey* gdkEvent =
            static_cast<const GdkEventKey*>(anEvent.mPluginEvent);
          event.keycode = gdkEvent->hardware_keycode;
          event.state = gdkEvent->state;
          switch (anEvent.mMessage)
            {
            case eKeyDown:
              // Handle eKeyDown for modifier key presses
              // For non-modifiers we get eKeyPress
              if (gdkEvent->is_modifier)
                event.type = XKeyPress;
              break;
            case eKeyPress:
              event.type = XKeyPress;
              break;
            case eKeyUp:
              event.type = KeyRelease;
              break;
            default:
              break;
            }
#endif

          // Information that could be obtained from pluginEvent but we may not
          // want to promise to provide:
          event.subwindow = X11None;
          event.x = 0;
          event.y = 0;
          event.x_root = -1;
          event.y_root = -1;
          event.same_screen = False;
        }
      else
        {
          // If we need to send synthesized key events, then
          // DOMKeyCodeToGdkKeyCode(keyEvent.keyCode) and
          // gdk_keymap_get_entries_for_keyval will be useful, but the
          // mappings will not be unique.
          NS_WARNING("Synthesized key event not sent to plugin");
        }
      break;

    default:
      switch (anEvent.mMessage) {
        case eFocus:
        case eBlur:
          {
            XFocusChangeEvent &event = pluginEvent.xfocus;
            event.type = anEvent.mMessage == eFocus ? FocusIn : FocusOut;
            // information lost:
            event.mode = -1;
            event.detail = NotifyDetailNone;
          }
          break;
        default:
          break;
      }
    }

  if (!pluginEvent.type) {
    return rv;
  }

  // Fill in (useless) generic event information.
  XAnyEvent& event = pluginEvent.xany;
  event.display = widget ?
    static_cast<Display*>(widget->GetNativeData(NS_NATIVE_DISPLAY)) : nullptr;
  event.window = X11None; // not a real window
  // information lost:
  event.serial = 0;
  event.send_event = False;

  int16_t response = kNPEventNotHandled;
  mInstance->HandleEvent(&pluginEvent, &response, NS_PLUGIN_CALL_SAFE_TO_REENTER_GECKO);
  if (response == kNPEventHandled)
    rv = nsEventStatus_eConsumeNoDefault;
#endif

  return rv;
}

nsresult
nsPluginInstanceOwner::Destroy()
{
  SetFrame(nullptr);

  nsCOMPtr<nsIContent> content = do_QueryReferent(mContent);

  // unregister context menu listener
  if (mCXMenuListener) {
    mCXMenuListener->Destroy(content);
    mCXMenuListener = nullptr;
  }

  content->RemoveEventListener(NS_LITERAL_STRING("focus"), this, false);
  content->RemoveEventListener(NS_LITERAL_STRING("blur"), this, false);
  content->RemoveEventListener(NS_LITERAL_STRING("mouseup"), this, false);
  content->RemoveEventListener(NS_LITERAL_STRING("mousedown"), this, false);
  content->RemoveEventListener(NS_LITERAL_STRING("mousemove"), this, false);
  content->RemoveEventListener(NS_LITERAL_STRING("click"), this, false);
  content->RemoveEventListener(NS_LITERAL_STRING("dblclick"), this, false);
  content->RemoveEventListener(NS_LITERAL_STRING("mouseover"), this, false);
  content->RemoveEventListener(NS_LITERAL_STRING("mouseout"), this, false);
  content->RemoveEventListener(NS_LITERAL_STRING("keypress"), this, true);
  content->RemoveEventListener(NS_LITERAL_STRING("keydown"), this, true);
  content->RemoveEventListener(NS_LITERAL_STRING("keyup"), this, true);
  content->RemoveEventListener(NS_LITERAL_STRING("drop"), this, true);
  content->RemoveEventListener(NS_LITERAL_STRING("drag"), this, true);
  content->RemoveEventListener(NS_LITERAL_STRING("dragenter"), this, true);
  content->RemoveEventListener(NS_LITERAL_STRING("dragover"), this, true);
  content->RemoveEventListener(NS_LITERAL_STRING("dragleave"), this, true);
  content->RemoveEventListener(NS_LITERAL_STRING("dragexit"), this, true);
  content->RemoveEventListener(NS_LITERAL_STRING("dragstart"), this, true);
  content->RemoveEventListener(NS_LITERAL_STRING("dragend"), this, true);
  content->RemoveSystemEventListener(NS_LITERAL_STRING("compositionstart"),
                                     this, true);
  content->RemoveSystemEventListener(NS_LITERAL_STRING("compositionend"),
                                     this, true);
  content->RemoveSystemEventListener(NS_LITERAL_STRING("text"), this, true);

  if (mWidget) {
    if (mPluginWindow) {
      mPluginWindow->SetPluginWidget(nullptr);
    }

    nsCOMPtr<nsIPluginWidget> pluginWidget = do_QueryInterface(mWidget);
    if (pluginWidget) {
      pluginWidget->SetPluginInstanceOwner(nullptr);
    }
    mWidget->Destroy();
  }

  return NS_OK;
}

// Paints are handled differently, so we just simulate an update event.

#if defined(MOZ_X11)
void nsPluginInstanceOwner::Paint(gfxContext* aContext,
                                  const gfxRect& aFrameRect,
                                  const gfxRect& aDirtyRect)
{
  if (!mInstance || !mPluginFrame)
    return;

  // to provide crisper and faster drawing.
  gfxRect pluginRect = aFrameRect;
  if (aContext->UserToDevicePixelSnapped(pluginRect)) {
    pluginRect = aContext->DeviceToUser(pluginRect);
  }

  // Round out the dirty rect to plugin pixels to ensure the plugin draws
  // enough pixels for interpolation to device pixels.
  gfxRect dirtyRect = aDirtyRect - pluginRect.TopLeft();
  dirtyRect.RoundOut();

  // Plugins can only draw an integer number of pixels.
  //
  // With translation-only transformation matrices, pluginRect is already
  // pixel-aligned.
  //
  // With more complex transformations, modifying the scales in the
  // transformation matrix could retain subpixel accuracy and let the plugin
  // draw a suitable number of pixels for interpolation to device pixels in
  // Renderer::Draw, but such cases are not common enough to warrant the
  // effort now.
  nsIntSize pluginSize(NS_lround(pluginRect.width),
                       NS_lround(pluginRect.height));

  // Determine what the plugin needs to draw.
  nsIntRect pluginDirtyRect(int32_t(dirtyRect.x),
                            int32_t(dirtyRect.y),
                            int32_t(dirtyRect.width),
                            int32_t(dirtyRect.height));
  if (!pluginDirtyRect.
      IntersectRect(nsIntRect(0, 0, pluginSize.width, pluginSize.height),
                    pluginDirtyRect))
    return;

  NPWindow* window;
  GetWindow(window);

  uint32_t rendererFlags = 0;
  if (!mFlash10Quirks) {
    rendererFlags |=
      Renderer::DRAW_SUPPORTS_CLIP_RECT |
      Renderer::DRAW_SUPPORTS_ALTERNATE_VISUAL;
  }

  bool transparent;
  mInstance->IsTransparent(&transparent);
  if (!transparent)
    rendererFlags |= Renderer::DRAW_IS_OPAQUE;

  // Renderer::Draw() draws a rectangle with top-left at the aContext origin.
  gfxContextAutoSaveRestore autoSR(aContext);
  aContext->SetMatrix(
    aContext->CurrentMatrix().Translate(pluginRect.TopLeft()));

  Renderer renderer(window, this, pluginSize, pluginDirtyRect);

  Display* dpy = mozilla::DefaultXDisplay();
  Screen* screen = DefaultScreenOfDisplay(dpy);
  Visual* visual = DefaultVisualOfScreen(screen);

  renderer.Draw(aContext, nsIntSize(window->width, window->height),
                rendererFlags, screen, visual);
}
nsresult
nsPluginInstanceOwner::Renderer::DrawWithXlib(cairo_surface_t* xsurface,
                                              nsIntPoint offset,
                                              nsIntRect *clipRects,
                                              uint32_t numClipRects)
{
  Screen *screen = cairo_xlib_surface_get_screen(xsurface);
  Colormap colormap;
  Visual* visual;
  if (!gfxXlibSurface::GetColormapAndVisual(xsurface, &colormap, &visual)) {
    NS_ERROR("Failed to get visual and colormap");
    return NS_ERROR_UNEXPECTED;
  }

  nsNPAPIPluginInstance *instance = mInstanceOwner->mInstance;
  if (!instance)
    return NS_ERROR_FAILURE;

  // See if the plugin must be notified of new window parameters.
  bool doupdatewindow = false;

  if (mWindow->x != offset.x || mWindow->y != offset.y) {
    mWindow->x = offset.x;
    mWindow->y = offset.y;
    doupdatewindow = true;
  }

  if (nsIntSize(mWindow->width, mWindow->height) != mPluginSize) {
    mWindow->width = mPluginSize.width;
    mWindow->height = mPluginSize.height;
    doupdatewindow = true;
  }

  // The clip rect is relative to drawable top-left.
  NS_ASSERTION(numClipRects <= 1, "We don't support multiple clip rectangles!");
  nsIntRect clipRect;
  if (numClipRects) {
    clipRect.x = clipRects[0].x;
    clipRect.y = clipRects[0].y;
    clipRect.width  = clipRects[0].width;
    clipRect.height = clipRects[0].height;
    // NPRect members are unsigned, but clip rectangles should be contained by
    // the surface.
    NS_ASSERTION(clipRect.x >= 0 && clipRect.y >= 0,
                 "Clip rectangle offsets are negative!");
  }
  else {
    clipRect.x = offset.x;
    clipRect.y = offset.y;
    clipRect.width  = mWindow->width;
    clipRect.height = mWindow->height;
    // Don't ask the plugin to draw outside the drawable.
    // This also ensures that the unsigned clip rectangle offsets won't be -ve.
    clipRect.IntersectRect(clipRect,
                           nsIntRect(0, 0,
                                     cairo_xlib_surface_get_width(xsurface),
                                     cairo_xlib_surface_get_height(xsurface)));
  }

  NPRect newClipRect;
  newClipRect.left = clipRect.x;
  newClipRect.top = clipRect.y;
  newClipRect.right = clipRect.XMost();
  newClipRect.bottom = clipRect.YMost();
  if (mWindow->clipRect.left    != newClipRect.left   ||
      mWindow->clipRect.top     != newClipRect.top    ||
      mWindow->clipRect.right   != newClipRect.right  ||
      mWindow->clipRect.bottom  != newClipRect.bottom) {
    mWindow->clipRect = newClipRect;
    doupdatewindow = true;
  }

  NPSetWindowCallbackStruct* ws_info =
    static_cast<NPSetWindowCallbackStruct*>(mWindow->ws_info);
#ifdef MOZ_X11
  if (ws_info->visual != visual || ws_info->colormap != colormap) {
    ws_info->visual = visual;
    ws_info->colormap = colormap;
    ws_info->depth = gfxXlibSurface::DepthOfVisual(screen, visual);
    doupdatewindow = true;
  }
#endif

  {
    if (doupdatewindow)
      instance->SetWindow(mWindow);
  }

  // Translate the dirty rect to drawable coordinates.
  nsIntRect dirtyRect = mDirtyRect + offset;
  if (mInstanceOwner->mFlash10Quirks) {
    // Work around a bug in Flash up to 10.1 d51 at least, where expose event
    // top left coordinates within the plugin-rect and not at the drawable
    // origin are misinterpreted.  (We can move the top left coordinate
    // provided it is within the clipRect.)
    dirtyRect.SetRect(offset.x, offset.y,
                      mDirtyRect.XMost(), mDirtyRect.YMost());
  }
  // Intersect the dirty rect with the clip rect to ensure that it lies within
  // the drawable.
  if (!dirtyRect.IntersectRect(dirtyRect, clipRect))
    return NS_OK;

  {
    XEvent pluginEvent = XEvent();
    XGraphicsExposeEvent& exposeEvent = pluginEvent.xgraphicsexpose;
    // set the drawing info
    exposeEvent.type = GraphicsExpose;
    exposeEvent.display = DisplayOfScreen(screen);
    exposeEvent.drawable = cairo_xlib_surface_get_drawable(xsurface);
    exposeEvent.x = dirtyRect.x;
    exposeEvent.y = dirtyRect.y;
    exposeEvent.width  = dirtyRect.width;
    exposeEvent.height = dirtyRect.height;
    exposeEvent.count = 0;
    // information not set:
    exposeEvent.serial = 0;
    exposeEvent.send_event = False;
    exposeEvent.major_code = 0;
    exposeEvent.minor_code = 0;

    instance->HandleEvent(&pluginEvent, nullptr);
  }
  return NS_OK;
}
#endif

nsresult nsPluginInstanceOwner::Init(nsIContent* aContent)
{
  mLastEventloopNestingLevel = GetEventloopNestingLevel();

  mContent = do_GetWeakReference(aContent);

  // Get a frame, don't reflow. If a reflow was necessary it should have been
  // done at a higher level than this (content).
  nsIFrame* frame = aContent->GetPrimaryFrame();
  nsIObjectFrame* iObjFrame = do_QueryFrame(frame);
  nsPluginFrame* objFrame =  static_cast<nsPluginFrame*>(iObjFrame);
  if (objFrame) {
    SetFrame(objFrame);
    // Some plugins require a specific sequence of shutdown and startup when
    // a page is reloaded. Shutdown happens usually when the last instance
    // is destroyed. Here we make sure the plugin instance in the old
    // document is destroyed before we try to create the new one.
    objFrame->PresContext()->EnsureVisible();
  } else {
    NS_NOTREACHED("Should not be initializing plugin without a frame");
    return NS_ERROR_FAILURE;
  }

  // register context menu listener
  mCXMenuListener = new nsPluginDOMContextMenuListener(aContent);

  aContent->AddEventListener(NS_LITERAL_STRING("focus"), this, false,
                             false);
  aContent->AddEventListener(NS_LITERAL_STRING("blur"), this, false,
                             false);
  aContent->AddEventListener(NS_LITERAL_STRING("mouseup"), this, false,
                             false);
  aContent->AddEventListener(NS_LITERAL_STRING("mousedown"), this, false,
                             false);
  aContent->AddEventListener(NS_LITERAL_STRING("mousemove"), this, false,
                             false);
  aContent->AddEventListener(NS_LITERAL_STRING("click"), this, false,
                             false);
  aContent->AddEventListener(NS_LITERAL_STRING("dblclick"), this, false,
                             false);
  aContent->AddEventListener(NS_LITERAL_STRING("mouseover"), this, false,
                             false);
  aContent->AddEventListener(NS_LITERAL_STRING("mouseout"), this, false,
                             false);
  aContent->AddEventListener(NS_LITERAL_STRING("keypress"), this, true);
  aContent->AddEventListener(NS_LITERAL_STRING("keydown"), this, true);
  aContent->AddEventListener(NS_LITERAL_STRING("keyup"), this, true);
  aContent->AddEventListener(NS_LITERAL_STRING("drop"), this, true);
  aContent->AddEventListener(NS_LITERAL_STRING("drag"), this, true);
  aContent->AddEventListener(NS_LITERAL_STRING("dragenter"), this, true);
  aContent->AddEventListener(NS_LITERAL_STRING("dragover"), this, true);
  aContent->AddEventListener(NS_LITERAL_STRING("dragleave"), this, true);
  aContent->AddEventListener(NS_LITERAL_STRING("dragexit"), this, true);
  aContent->AddEventListener(NS_LITERAL_STRING("dragstart"), this, true);
  aContent->AddEventListener(NS_LITERAL_STRING("dragend"), this, true);
  aContent->AddSystemEventListener(NS_LITERAL_STRING("compositionstart"),
    this, true);
  aContent->AddSystemEventListener(NS_LITERAL_STRING("compositionend"), this,
    true);
  aContent->AddSystemEventListener(NS_LITERAL_STRING("text"), this, true);

  return NS_OK;
}

void* nsPluginInstanceOwner::GetPluginPort()
{
  void* result = nullptr;
  if (mWidget) {
      result = mWidget->GetNativeData(NS_NATIVE_PLUGIN_PORT); // HWND/gdk window
  }

  return result;
}

void nsPluginInstanceOwner::ReleasePluginPort(void * pluginPort)
{
}

NS_IMETHODIMP nsPluginInstanceOwner::CreateWidget(void)
{
  NS_ENSURE_TRUE(mPluginWindow, NS_ERROR_NULL_POINTER);

  nsresult rv = NS_ERROR_FAILURE;

  // Can't call this twice!
  if (mWidget) {
    NS_WARNING("Trying to create a plugin widget twice!");
    return NS_ERROR_FAILURE;
  }

  bool windowless = false;
  mInstance->IsWindowless(&windowless);
  if (!windowless) {
    // Try to get a parent widget, on some platforms widget creation will fail without
    // a parent.
    nsCOMPtr<nsIWidget> parentWidget;
    nsIDocument *doc = nullptr;
    nsCOMPtr<nsIContent> content = do_QueryReferent(mContent);
    if (content) {
      doc = content->OwnerDoc();
      parentWidget = nsContentUtils::WidgetForDocument(doc);
      // If we're running in the content process, we need a remote widget created in chrome.
      if (XRE_IsContentProcess()) {
        if (nsCOMPtr<nsPIDOMWindowOuter> window = doc->GetWindow()) {
          if (nsCOMPtr<nsPIDOMWindowOuter> topWindow = window->GetTop()) {
            dom::TabChild* tc = dom::TabChild::GetFrom(topWindow);
            if (tc) {
              // This returns a PluginWidgetProxy which remotes a number of calls.
              rv = tc->CreatePluginWidget(parentWidget.get(), getter_AddRefs(mWidget));
              if (NS_FAILED(rv)) {
                return rv;
              }
            }
          }
        }
      }
    }

    // A failure here is terminal since we can't fall back on the non-e10s code
    // path below.
    if (!mWidget && XRE_IsContentProcess()) {
      return NS_ERROR_UNEXPECTED;
    }

    if (!mWidget) {
      // native (single process)
      mWidget = do_CreateInstance(kWidgetCID, &rv);
      nsWidgetInitData initData;
      initData.mWindowType = eWindowType_plugin;
      initData.mUnicode = false;
      initData.clipChildren = true;
      initData.clipSiblings = true;
      rv = mWidget->Create(parentWidget.get(), nullptr,
                           LayoutDeviceIntRect(0, 0, 0, 0), &initData);
      if (NS_FAILED(rv)) {
        mWidget->Destroy();
        mWidget = nullptr;
        return rv;
      }
    }

    mWidget->EnableDragDrop(true);
    mWidget->Show(false);
    mWidget->Enable(false);
  }

  if (mPluginFrame) {
    // nullptr widget is fine, will result in windowless setup.
    mPluginFrame->PrepForDrawing(mWidget);
  }

  if (windowless) {
    mPluginWindow->type = NPWindowTypeDrawable;

    // this needs to be a HDC according to the spec, but I do
    // not see the right way to release it so let's postpone
    // passing HDC till paint event when it is really
    // needed. Change spec?
    mPluginWindow->window = nullptr;
#ifdef MOZ_X11
    // Fill in the display field.
    NPSetWindowCallbackStruct* ws_info =
    static_cast<NPSetWindowCallbackStruct*>(mPluginWindow->ws_info);
    ws_info->display = DefaultXDisplay();

    nsAutoCString description;
    GetPluginDescription(description);
    NS_NAMED_LITERAL_CSTRING(flash10Head, "Shockwave Flash 10.");
    mFlash10Quirks = StringBeginsWith(description, flash10Head);
#endif
  } else if (mWidget) {
    // mPluginWindow->type is used in |GetPluginPort| so it must
    // be initialized first
    mPluginWindow->type = NPWindowTypeWindow;
    mPluginWindow->window = GetPluginPort();
    // tell the plugin window about the widget
    mPluginWindow->SetPluginWidget(mWidget);

    // tell the widget about the current plugin instance owner.
    nsCOMPtr<nsIPluginWidget> pluginWidget = do_QueryInterface(mWidget);
    if (pluginWidget) {
      pluginWidget->SetPluginInstanceOwner(this);
    }
  }

  mWidgetCreationComplete = true;

  return NS_OK;
}

void nsPluginInstanceOwner::UpdateWindowPositionAndClipRect(bool aSetWindow)
{
  if (!mPluginWindow)
    return;

  // For windowless plugins a non-empty clip rectangle will be
  // passed to the plugin during paint, an additional update
  // of the the clip rectangle here is not required
  if (aSetWindow && !mWidget && mPluginWindowVisible && !UseAsyncRendering())
    return;

  const NPWindow oldWindow = *mPluginWindow;

  bool windowless = (mPluginWindow->type == NPWindowTypeDrawable);
  nsIntPoint origin = mPluginFrame->GetWindowOriginInPixels(windowless);

  mPluginWindow->x = origin.x;
  mPluginWindow->y = origin.y;

  mPluginWindow->clipRect.left = 0;
  mPluginWindow->clipRect.top = 0;

  if (mPluginWindowVisible && mPluginDocumentActiveState) {
    mPluginWindow->clipRect.right = mPluginWindow->width;
    mPluginWindow->clipRect.bottom = mPluginWindow->height;
  } else {
    mPluginWindow->clipRect.right = 0;
    mPluginWindow->clipRect.bottom = 0;
  }

  if (!aSetWindow)
    return;

  if (mPluginWindow->x               != oldWindow.x               ||
      mPluginWindow->y               != oldWindow.y               ||
      mPluginWindow->clipRect.left   != oldWindow.clipRect.left   ||
      mPluginWindow->clipRect.top    != oldWindow.clipRect.top    ||
      mPluginWindow->clipRect.right  != oldWindow.clipRect.right  ||
      mPluginWindow->clipRect.bottom != oldWindow.clipRect.bottom) {
    CallSetWindow();
  }
}

void
nsPluginInstanceOwner::UpdateWindowVisibility(bool aVisible)
{
  mPluginWindowVisible = aVisible;
  UpdateWindowPositionAndClipRect(true);
}

void
nsPluginInstanceOwner::ResolutionMayHaveChanged()
{
  float zoomFactor = 1.0;
  GetCSSZoomFactor(&zoomFactor);
  if (zoomFactor != mLastCSSZoomFactor) {
    if (mInstance) {
      mInstance->CSSZoomFactorChanged(zoomFactor);
    }
    mLastCSSZoomFactor = zoomFactor;
  }

}

void
nsPluginInstanceOwner::UpdateDocumentActiveState(bool aIsActive)
{
  PROFILER_LABEL_FUNC(js::ProfileEntry::Category::OTHER);

  mPluginDocumentActiveState = aIsActive;
  UpdateWindowPositionAndClipRect(true);

  // We don't have a connection to PluginWidgetParent in the chrome
  // process when dealing with tab visibility changes, so this needs
  // to be forwarded over after the active state is updated. If we
  // don't hide plugin widgets in hidden tabs, the native child window
  // in chrome will remain visible after a tab switch.
  if (mWidget && XRE_IsContentProcess()) {
    mWidget->Show(aIsActive);
    mWidget->Enable(aIsActive);
  }
}

NS_IMETHODIMP
nsPluginInstanceOwner::CallSetWindow()
{
  if (!mWidgetCreationComplete) {
    // No widget yet, we can't run this code
    return NS_OK;
  }
  if (mPluginFrame) {
    mPluginFrame->CallSetWindow(false);
  } else if (mInstance) {
    if (UseAsyncRendering()) {
      mInstance->AsyncSetWindow(mPluginWindow);
    } else {
      mInstance->SetWindow(mPluginWindow);
    }
  }

  return NS_OK;
}

NS_IMETHODIMP
nsPluginInstanceOwner::GetContentsScaleFactor(double *result)
{
  NS_ENSURE_ARG_POINTER(result);
  double scaleFactor = 1.0;
  // On Mac, device pixels need to be translated to (and from) "display pixels"
  // for plugins. On other platforms, plugin coordinates are always in device
  // pixels.
  *result = scaleFactor;
  return NS_OK;
}

void
nsPluginInstanceOwner::GetCSSZoomFactor(float *result)
{
  nsCOMPtr<nsIContent> content = do_QueryReferent(mContent);
  nsIPresShell* presShell = nsContentUtils::FindPresShellForDocument(content->OwnerDoc());
  if (presShell) {
    *result = presShell->GetPresContext()->DeviceContext()->GetFullZoom();
  } else {
    *result = 1.0;
  }
}

void nsPluginInstanceOwner::SetFrame(nsPluginFrame *aFrame)
{
  // Don't do anything if the frame situation hasn't changed.
  if (mPluginFrame == aFrame) {
    return;
  }

  nsCOMPtr<nsIContent> content = do_QueryReferent(mContent);

  // If we already have a frame that is changing or going away...
  if (mPluginFrame) {
    if (content && content->OwnerDoc() && content->OwnerDoc()->GetWindow()) {
      nsCOMPtr<EventTarget> windowRoot = content->OwnerDoc()->GetWindow()->GetTopWindowRoot();
      if (windowRoot) {
        windowRoot->RemoveEventListener(NS_LITERAL_STRING("activate"),
                                              this, false);
        windowRoot->RemoveEventListener(NS_LITERAL_STRING("deactivate"),
                                              this, false);
        windowRoot->RemoveEventListener(NS_LITERAL_STRING("MozPerformDelayedBlur"),
                                              this, false);
      }
    }

    // Make sure the old frame isn't holding a reference to us.
    mPluginFrame->SetInstanceOwner(nullptr);
  }

  // Swap in the new frame (or no frame)
  mPluginFrame = aFrame;

  // Set up a new frame
  if (mPluginFrame) {
    mPluginFrame->SetInstanceOwner(this);
    // Can only call PrepForDrawing on an object frame once. Don't do it here unless
    // widget creation is complete. Doesn't matter if we actually have a widget.
    if (mWidgetCreationComplete) {
      mPluginFrame->PrepForDrawing(mWidget);
    }
    mPluginFrame->FixupWindow(mPluginFrame->GetContentRectRelativeToSelf().Size());
    mPluginFrame->InvalidateFrame();

    nsFocusManager* fm = nsFocusManager::GetFocusManager();
    const nsIContent* content = aFrame->GetContent();
    if (fm && content) {
      mContentFocused = (content == fm->GetFocusedContent());
    }

    // Register for widget-focus events on the window root.
    if (content && content->OwnerDoc() && content->OwnerDoc()->GetWindow()) {
      nsCOMPtr<EventTarget> windowRoot = content->OwnerDoc()->GetWindow()->GetTopWindowRoot();
      if (windowRoot) {
        windowRoot->AddEventListener(NS_LITERAL_STRING("activate"),
                                           this, false, false);
        windowRoot->AddEventListener(NS_LITERAL_STRING("deactivate"),
                                           this, false, false);
        windowRoot->AddEventListener(NS_LITERAL_STRING("MozPerformDelayedBlur"),
                                           this, false, false);
      }
    }
  }
}

nsPluginFrame* nsPluginInstanceOwner::GetFrame()
{
  return mPluginFrame;
}

NS_IMETHODIMP nsPluginInstanceOwner::PrivateModeChanged(bool aEnabled)
{
  return mInstance ? mInstance->PrivateModeStateChanged(aEnabled) : NS_OK;
}

already_AddRefed<nsIURI> nsPluginInstanceOwner::GetBaseURI() const
{
  nsCOMPtr<nsIContent> content = do_QueryReferent(mContent);
  if (!content) {
    return nullptr;
  }
  return content->GetBaseURI();
}

// static
void
nsPluginInstanceOwner::GeneratePluginEvent(
  const WidgetCompositionEvent* aSrcCompositionEvent,
  WidgetCompositionEvent* aDistCompositionEvent)
{
}

// nsPluginDOMContextMenuListener class implementation

nsPluginDOMContextMenuListener::nsPluginDOMContextMenuListener(nsIContent* aContent)
{
  aContent->AddEventListener(NS_LITERAL_STRING("contextmenu"), this, true);
}

nsPluginDOMContextMenuListener::~nsPluginDOMContextMenuListener()
{
}

NS_IMPL_ISUPPORTS(nsPluginDOMContextMenuListener,
                  nsIDOMEventListener)

NS_IMETHODIMP
nsPluginDOMContextMenuListener::HandleEvent(nsIDOMEvent* aEvent)
{
  aEvent->PreventDefault(); // consume event

  return NS_OK;
}

void nsPluginDOMContextMenuListener::Destroy(nsIContent* aContent)
{
  // Unregister context menu listener
  aContent->RemoveEventListener(NS_LITERAL_STRING("contextmenu"), this, true);
}
