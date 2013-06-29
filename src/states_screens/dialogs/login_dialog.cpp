//  SuperTuxKart - a fun racing game with go-kart
//  Copyright (C) 2013 Glenn De Jonghe
//
//  This program is free software; you can redistribute it and/or
//  modify it under the terms of the GNU General Public License
//  as published by the Free Software Foundation; either version 3
//  of the License, or (at your option) any later version.
//
//  This program is distributed in the hope that it will be useful,
//  but WITHOUT ANY WARRANTY; without even the implied warranty of
//  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//  GNU General Public License for more details.
//
//  You should have received a copy of the GNU General Public License
//  along with this program; if not, write to the Free Software
//  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

#include "states_screens/dialogs/login_dialog.hpp"

#include <IGUIEnvironment.h>

#include "audio/sfx_manager.hpp"
#include "config/player.hpp"
#include "guiengine/engine.hpp"
#include "guiengine/widgets/button_widget.hpp"
#include "guiengine/widgets/text_box_widget.hpp"
#include "states_screens/state_manager.hpp"
#include "utils/translation.hpp"
#include "utils/string_utils.hpp"
#include "online/current_online_user.hpp"
#include "states_screens/dialogs/registration_dialog.hpp"


using namespace GUIEngine;
using namespace irr;
using namespace irr::gui;

// -----------------------------------------------------------------------------

LoginDialog::LoginDialog(const Message message_type) :
        ModalDialog(0.8f,0.8f)
{
    m_self_destroy = false;
    m_open_registration_dialog = false;
    m_reshow_current_screen = false;
    loadFromFile("online/login_dialog.stkgui");

    TextBoxWidget* textBox = getWidget<TextBoxWidget>("password");
    assert(textBox != NULL);
    textBox->setPasswordBox(true,L'*');

    textBox = getWidget<TextBoxWidget>("username");
    assert(textBox != NULL);
    textBox->setFocusForPlayer(PLAYER_ID_GAME_MASTER);

    LabelWidget * m_info_widget = getWidget<LabelWidget>("info");
    assert(m_info_widget != NULL);
    irr::core::stringw info;
    if (message_type == Normal)
        info =  _("Fill in your username and password. ");
    else if (message_type == Signing_In_Required)
        info =  _("You need to sign in to be able to use this feature. ");
    else if (message_type == Registration_Required)
        info =  _("You need to be a registered user to enjoy this feature! "
                  "If you do not have an account yet, you can sign up using the register icon at the bottom.");
    else
        info = "";
    if (message_type == Normal || message_type == Signing_In_Required)
        info += _("If you do not have an account yet, you can choose to sign in as a guest "
                  "or press the register icon at the bottom to gain access to all the features!");
    m_info_widget->setText(info, false);
}

// -----------------------------------------------------------------------------

LoginDialog::~LoginDialog()
{
}

// -----------------------------------------------------------------------------
void LoginDialog::beforeAddingWidgets()
{
    LabelWidget * m_message_widget = getWidget<LabelWidget>("message");
    assert(m_message_widget != NULL);
}

// -----------------------------------------------------------------------------

GUIEngine::EventPropagation LoginDialog::processEvent(const std::string& eventSource)
{
    if (eventSource == "cancel")
    {
        m_self_destroy = true;
        return GUIEngine::EVENT_BLOCK;
    }
    else if(eventSource == "sign_in")
    {
        const stringw username = getWidget<TextBoxWidget>("username")->getText().trim();
        const stringw password = getWidget<TextBoxWidget>("password")->getText().trim();
        stringw info = "";
        if(CurrentOnlineUser::get()->signIn(username,password,info))
        {
            m_reshow_current_screen = true;
            m_self_destroy = true;
        }
        else
        {
            sfx_manager->quickSound( "anvil" );
            Log::info("Login Dialog", "check1");
            irr::video::SColor red(255, 255, 0, 0);
            m_message_widget->setColor(red);
            m_message_widget->setText(info, false);
            Log::info("Login Dialog", "check2");
        }
        return GUIEngine::EVENT_BLOCK;
    }
    else if(eventSource == "sign_up")
    {
        m_open_registration_dialog = true;
        return GUIEngine::EVENT_BLOCK;
    }
    return GUIEngine::EVENT_LET;
}

// -----------------------------------------------------------------------------

void LoginDialog::onEnterPressedInternal()
{
    //If enter was pressed while "cancel" nor "signup" was selected, then interpret as "signin" press.
    const int playerID = PLAYER_ID_GAME_MASTER;
    ButtonWidget* cancelButton = getWidget<ButtonWidget>("cancel");
    assert(cancelButton != NULL);
    ButtonWidget* registerButton = getWidget<ButtonWidget>("sign_up");
    assert(registerButton != NULL);
    if (!GUIEngine::isFocusedForPlayer(cancelButton, playerID)          &&
        !GUIEngine::isFocusedForPlayer(registerButton, playerID))
    {
        processEvent("sign_in");
    }
}

// -----------------------------------------------------------------------------

void LoginDialog::onUpdate(float dt)
{
    //If we want to open the registration dialog, we need to close this one first
    m_open_registration_dialog && (m_self_destroy = true);

    // It's unsafe to delete from inside the event handler so we do it here
    if (m_self_destroy)
    {
        ModalDialog::dismiss();
        if (m_reshow_current_screen)
            GUIEngine::reshowCurrentScreen();
        if (m_open_registration_dialog)
            new RegistrationDialog(0.8f, 0.9f);

    }


}