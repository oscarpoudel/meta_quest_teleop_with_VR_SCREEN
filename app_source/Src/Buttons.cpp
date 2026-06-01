//
// Created by Jedrzej on 12/16/2020.
//

#include "Buttons.h"
#include "Misc/Log.h"

namespace OVRFW {
    void Buttons::update_buttons(
            ovrInputStateTrackedRemote remoteInputState, const ovrHandedness controllerHand) {
        if (controllerHand == VRAPI_HAND_LEFT) {
            leftRemoteInputState_ = remoteInputState;
        }
        else {
            rightRemoteInputState_ = remoteInputState;
        }
    }

    std::string Buttons::current_to_string(char side) const {
        std::string text = "";
        // right hand
        if (side == 'r') {
            text += "R,"; // tell that right hand is available
            ovrInputStateTrackedRemote remoteInputState = rightRemoteInputState_;
            if (remoteInputState.Buttons & ovrButton_A)
                text += "A,";
            if (remoteInputState.Buttons & ovrButton_B)
                text += "B,";
            if (remoteInputState.Buttons & ovrButton_Trigger)
                text += "RTr,";
            if (remoteInputState.Buttons & ovrButton_GripTrigger)
                text += "RG,";
            if (remoteInputState.Touches & ovrTouch_ThumbUp)
                text += "RThU,";
            if (remoteInputState.Buttons & ovrButton_Joystick)
                text += "RJ,";
            text += "rightJS " + std::to_string(remoteInputState.Joystick.x) + " " +
                    std::to_string(remoteInputState.Joystick.y) + ",";
            text += "rightTrig " + std::to_string(remoteInputState.IndexTrigger) + ",";
            text += "rightGrip " + std::to_string(remoteInputState.GripTrigger);
        }
        // left hand
        else if (side == 'l') {
            text += "L,"; // tell that left hand is available
            ovrInputStateTrackedRemote remoteInputState = leftRemoteInputState_;
            if (remoteInputState.Buttons & ovrButton_X)
                text += "X,";
            if (remoteInputState.Buttons & ovrButton_Y)
                text += "Y,";
            if (remoteInputState.Buttons & ovrButton_Trigger)
                text += "LTr,";
            if (remoteInputState.Buttons & ovrButton_GripTrigger)
                text += "LG,";
            if (remoteInputState.Touches & ovrTouch_ThumbUp)
                text += "LThU,";
            if (remoteInputState.Buttons & ovrButton_Joystick)
                text += "LJ,";
            text += "leftJS " + std::to_string(remoteInputState.Joystick.x) + " " +
                    std::to_string(remoteInputState.Joystick.y) + ",";
            text += "leftTrig " + std::to_string(remoteInputState.IndexTrigger) + ",";
            text += "leftGrip " + std::to_string(remoteInputState.GripTrigger);
        }
        else {
            __android_log_print(ANDROID_LOG_ERROR, "RAILOculusTeleop", "Side value neither l nor r in Buttons::current_to_string");
        }
        return text;
    }

    bool Buttons::is_button_pressed(char side, const char* buttonName) const {
        const ovrInputStateTrackedRemote remoteInputState = side == 'l'
            ? leftRemoteInputState_
            : rightRemoteInputState_;
        if (buttonName == nullptr) {
            return false;
        }
        const std::string name(buttonName);
        if (side == 'r') {
            if (name == "A") return (remoteInputState.Buttons & ovrButton_A) != 0;
            if (name == "B") return (remoteInputState.Buttons & ovrButton_B) != 0;
            if (name == "RJ") return (remoteInputState.Buttons & ovrButton_Joystick) != 0;
        } else if (side == 'l') {
            if (name == "X") return (remoteInputState.Buttons & ovrButton_X) != 0;
            if (name == "Y") return (remoteInputState.Buttons & ovrButton_Y) != 0;
            if (name == "LJ") return (remoteInputState.Buttons & ovrButton_Joystick) != 0;
        }
        return false;
    }
}
