#include <unistd.h>
#include <memory>
#include <iostream>
#include <napi.h>
#include <proc/readproc.h>
#include <xcb/composite.h>
#include <thread>
#include "os.h"
#include "linux/x11.h"
#include "linux/shm.h"

using namespace priv_os_x11;

void WindowThread(xcb_window_t);

void OSWindow::SetBounds(JSRectangle bounds) {
	ensureConnection();
	xcb_query_tree_cookie_t cookie = xcb_query_tree(connection, this->handle);
	xcb_query_tree_reply_t* reply = xcb_query_tree_reply(connection, cookie, NULL);
	if (reply) {
		if (bounds.width > 0 && bounds.height > 0) {
			int32_t config[] = {
				bounds.x,
				bounds.y,
				bounds.width,
				bounds.height,
			};
			xcb_configure_window(connection, reply->parent, XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y | XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT, &config);
			int32_t childConfig[] = { 0, 0, bounds.width, bounds.height };
			xcb_configure_window(connection, this->handle, XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y | XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT, &childConfig);
		} else {
			int32_t config[] = {
				bounds.x,
				bounds.y,
			};
			xcb_configure_window(connection, reply->parent, XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y, &config);
		}
		xcb_flush(connection);
		free(reply);
	}
}

JSRectangle OSWindow::GetBounds() {
	ensureConnection();
	xcb_query_tree_cookie_t cookie = xcb_query_tree(connection, this->handle);
	xcb_query_tree_reply_t* reply = xcb_query_tree_reply(connection, cookie, NULL);
	if (!reply) {
		return JSRectangle();
	}
	xcb_get_geometry_cookie_t cookie2 = xcb_get_geometry(connection, reply->parent);
	free(reply);
	xcb_get_geometry_reply_t* reply2 = xcb_get_geometry_reply(connection, cookie2, NULL);
	if (!reply2) {
		return JSRectangle();
	}
	auto result = JSRectangle(reply2->x, reply2->y, reply2->width, reply2->height);
	free(reply2);
	return result;
}

JSRectangle OSWindow::GetClientBounds() {
	ensureConnection();
	xcb_get_geometry_cookie_t cookie = xcb_get_geometry(connection, this->handle);
	xcb_get_geometry_reply_t* reply = xcb_get_geometry_reply(connection, cookie, NULL);
	if (!reply) {
		return JSRectangle();
	}

	auto x = reply->x;
	auto y = reply->y;
	auto width = reply->width;
	auto height = reply->height;
	auto window = this->handle;
	while (true) {
		xcb_query_tree_cookie_t cookieTree = xcb_query_tree(connection, window);
		xcb_query_tree_reply_t* replyTree = xcb_query_tree_reply(connection, cookieTree, NULL);
		if (replyTree == NULL || replyTree->parent == reply->root) {
			break;
		}
		window = replyTree->parent;
		free(reply);
		free(replyTree);

		cookie = xcb_get_geometry(connection, window);
		reply = xcb_get_geometry_reply(connection, cookie, NULL);
		if (reply == NULL) {
			break;
		}

		x += reply->x;
		y += reply->y;
	}
	free(reply);
	return JSRectangle(x, y, width, height);
}

bool OSWindow::IsValid() {
	if (!this->handle) {
		return false;
	}

	ensureConnection();
	xcb_get_geometry_cookie_t cookie = xcb_get_geometry_unchecked(connection, this->handle);
	std::unique_ptr<xcb_get_geometry_reply_t, decltype(&free)> reply { xcb_get_geometry_reply(connection, cookie, NULL), &free };
	return !!reply;
}

std::string OSWindow::GetTitle() {
	ensureConnection();
	xcb_get_property_cookie_t cookie = xcb_get_property_unchecked(connection, 0, this->handle, XCB_ATOM_WM_NAME, XCB_ATOM_STRING, 0, 100);
	std::unique_ptr<xcb_get_property_reply_t, decltype(&free)> reply { xcb_get_property_reply(connection, cookie, NULL), &free };
	if (!reply) {
		return std::string();
	}

	char* title = reinterpret_cast<char*>(xcb_get_property_value(reply.get()));
	int length = xcb_get_property_value_length(reply.get());

	return std::string(title, length);
}

Napi::Value OSWindow::ToJS(Napi::Env env) {
	return Napi::BigInt::New(env, (uint64_t) this->handle);
}

bool OSWindow::operator==(const OSWindow& other) const {
	return this->handle == other.handle;
}

bool OSWindow::operator<(const OSWindow& other) const {
	return this->handle < other.handle;
}

OSWindow OSWindow::FromJsValue(const Napi::Value jsval) {
	auto handle = jsval.As<Napi::BigInt>();
	bool lossless;
	xcb_window_t handleint = handle.Uint64Value(&lossless);
	if (!lossless) {
		Napi::RangeError::New(jsval.Env(), "Invalid handle").ThrowAsJavaScriptException();
	}
	return OSWindow(handleint);
}

void GetRsHandlesRecursively(const xcb_window_t window, std::vector<OSWindow>* out, unsigned int* deepest, unsigned int depth = 0) {
	const uint32_t long_length = 4096;
	xcb_query_tree_cookie_t cookie = xcb_query_tree(connection, window);
	xcb_query_tree_reply_t* reply = xcb_query_tree_reply(connection, cookie, NULL);
	if (reply == NULL) {
		return;
	}

	xcb_window_t* children = xcb_query_tree_children(reply);

	for (auto i = 0; i < xcb_query_tree_children_length(reply); i++) {
		// First, check WM_CLASS for either "RuneScape" or "steam_app_1343400"
		xcb_window_t child = children[i];
		xcb_get_property_cookie_t cookieProp = xcb_get_property(connection, 0, child, XCB_ATOM_WM_CLASS, XCB_ATOM_STRING, 0, long_length);
		xcb_get_property_reply_t* replyProp = xcb_get_property_reply(connection, cookieProp, NULL);
		if (replyProp != NULL) {
			auto len = xcb_get_property_value_length(replyProp);
			// if len == long_length then that means we didn't read the whole property, so discard.
			if (len > 0 && (uint32_t)len < long_length) {
				char buffer[long_length] = { 0 };
				memcpy(buffer, xcb_get_property_value(replyProp), len);
				// first is instance name, then class name - both null terminated. we want class name.
				const char* classname = buffer + strlen(buffer) + 1;
				if (strcmp(classname, "RuneScape") == 0 || strcmp(classname, "steam_app_1343400") == 0) {
					// Now, only take this if it's one of the deepest instances found so far
					if (depth > *deepest) {
						out->clear();
						out->push_back(child);
						*deepest = depth;
					} else if (depth == *deepest) {
						out->push_back(child);
					}
				}
			}
		}
		free(replyProp);
		GetRsHandlesRecursively(child, out, deepest, depth + 1);
	}

	free(reply);
}

std::vector<OSWindow> OSGetRsHandles() {
	ensureConnection();
	std::vector<OSWindow> out;
	unsigned int deepest = 0;
	GetRsHandlesRecursively(rootWindow, &out, &deepest);
	return out;
}

void OSSetWindowParent(OSWindow window, OSWindow parent) {
	ensureConnection();
	
	// Query the tree of the game window
	xcb_query_tree_cookie_t cookieTree = xcb_query_tree(connection, parent.handle);
	xcb_query_tree_reply_t* tree = xcb_query_tree_reply(connection, cookieTree, NULL);
	xcb_get_geometry_cookie_t cookieGeometry = xcb_get_geometry(connection, window.handle);
	xcb_get_geometry_reply_t* geometry = xcb_get_geometry_reply(connection, cookieGeometry, NULL);

	if (tree && tree->parent != tree->root) {
		const uint32_t values[] = { 1, 33554431 };
		xcb_window_t id = xcb_generate_id(connection);
		// Create a new window, parented to the game's parent, with the game view's geometry and OverrideRedirect
		xcb_create_window(connection, XCB_COPY_FROM_PARENT, id, tree->parent, 0, 0, geometry->width, geometry->height,
							0, XCB_WINDOW_CLASS_INPUT_OUTPUT, XCB_COPY_FROM_PARENT, XCB_CW_OVERRIDE_REDIRECT | XCB_CW_EVENT_MASK, &values);

		// Map our new frame
		xcb_map_window(connection, id);

		// Set OverrideRedirect on the electron window, and request all its events
		xcb_change_window_attributes(connection, window.handle, XCB_CW_OVERRIDE_REDIRECT | XCB_CW_EVENT_MASK, &values);

		// Parent the electron window to our frame
		xcb_reparent_window(connection, window.handle, id, 0, 0);

		new std::thread(WindowThread, id);
	}

	free(tree);
}

void OSCaptureDesktopMulti(OSWindow wnd, vector<CaptureRect> rects) {
	ensureConnection();
	XShmCapture acquirer(connection, rootWindow);
	//TODO double check and document desktop 0 special case
	auto offset = wnd.GetClientBounds();

	for (CaptureRect &rect : rects) {
		acquirer.copy(reinterpret_cast<char*>(rect.data), rect.size, rect.rect.x + offset.x, rect.rect.y + offset.y, rect.rect.width, rect.rect.height);
	}
}

void OSCaptureWindowMulti(OSWindow wnd, vector<CaptureRect> rects) {
	ensureConnection();
	xcb_composite_redirect_window(connection, wnd.handle, XCB_COMPOSITE_REDIRECT_AUTOMATIC);
	xcb_pixmap_t pixId = xcb_generate_id(connection);
	xcb_composite_name_window_pixmap(connection, wnd.handle, pixId);

	xcb_get_geometry_cookie_t cookie = xcb_get_geometry(connection, pixId);
	xcb_get_geometry_reply_t* reply = xcb_get_geometry_reply(connection, cookie, NULL);
	if (!reply) {
		xcb_free_pixmap(connection, pixId);
		return;
	}

	XShmCapture acquirer(connection, pixId);

	for (CaptureRect &rect : rects) {
		acquirer.copy(reinterpret_cast<char*>(rect.data), rect.size, rect.rect.x, rect.rect.y, rect.rect.width, rect.rect.height);
	}

	free(reply);
	xcb_free_pixmap(connection, pixId);
}

void OSCaptureMulti(OSWindow wnd, CaptureMode mode, vector<CaptureRect> rects, Napi::Env env) {
	switch (mode) {
		case CaptureMode::Desktop: {
			OSCaptureDesktopMulti(wnd, rects);
			break;
		}
		case CaptureMode::Window:
			OSCaptureWindowMulti(wnd, rects);
			break;
		default:
			throw Napi::RangeError::New(env, "Capture mode not supported");
	}
}

OSWindow OSGetActiveWindow() {
	xcb_get_property_cookie_t cookie = xcb_ewmh_get_active_window(&ewmhConnection, 0);
	xcb_window_t window;
	if (xcb_ewmh_get_active_window_reply(&ewmhConnection, cookie, &window, NULL) == 0) {
		return OSWindow(0);
	}

	return OSWindow(window);
}
	

void OSNewWindowListener(OSWindow wnd, WindowEventType type, Napi::Function cb) {

}

void OSRemoveWindowListener(OSWindow wnd, WindowEventType type, Napi::Function cb) {

}

void WindowThread(xcb_window_t window) {
	xcb_generic_event_t* event;
	while (true) {
		event = xcb_wait_for_event(connection);
		std::cout << "Event Type: " << (int)event->response_type << std::endl;
	}
}
