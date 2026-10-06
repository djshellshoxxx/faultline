# Items to include in all VST projects

The following items need to be included in all VSTs


# Help File

-a help section accessable from within the VST that explains each feature
-explaining how to use the VST and the workflow involved
-a general discription of the GUI and what the controls do
-the version number
-the license
-link to github if open source
-link to homepage
-link to support email
-troubleshooting guide if the installation is broken (explain how to manually install and uninstall)
-troubleshooting guide for where to put presets files to get them to work
-in the help section there should be a debug button which does the following (see next section)

# Custom Icon

The Icon for the exe must be unique to every project


# Presets

A bank of presets with discriptive names is required


#A reset button

The reset button will put all settings and functions back to the default settings


#save/save as and open and option functions

These should be accessed via a dropdown menu. WIth these you can save your preset to a file for later retrieval and open them whenever you want. The Options menu item will bring you to the options page which will let you turn tool tips on or off as well as let you choose your midi and audio card functions. also include an 'open location in explorer' option.


#Export audio

This option will let you export the audio you have made to a wav file. This function is only for instruments and not for effects except for special functions. the user should have basic options for the quality of the saved file. when the file is saved successfully a popup should say so and indicate the location of the created file and the length and name and quality.

#import and export midi

this feature is only relevant if the vst has sequencing features or similar. There should be an option to export audio as midi as well as import midi, but only when this function makes sense. It doesnt make sense to include this in an effects vst.


# Right click functionality

Right clicking on any controll will let you map to midi or reset the control to default or set the control to a spicific number. Other rightclick functions may be added in the future.

#drag and drop

Whenever a vst allows importing of a sample it should allow for drag and drop functionality


# Hover-over tool tips

Self explanitory. this is to help the user understand and learn the controls


# A Randomize feature

The "Randomize" button is a feature that will randomize the settings to give you a unique sound or effect every time. Each time the random button is clicked it will give you a totally new set of settings. After the frist press, each additional press will reset all settings before randomizing.


# Other Notes about the build

Keep in mind that this build may become the pro version, and a free version might be made in future builds. Also, instruments may also have an effects version made in the future. At the end always test every possible combination of setting and throughly debug and attempt to find logic errors.

Always backup and push to a private github whenever a milestone is reached.

Do not come back with an incomplete build. 

Tripple test all features and functions with every combination of setting in program and contnue to bug fix until bug free. 

Once all bugs are fixed do a deep analysis for logic errors and work flow errors in the gui.

Create the vst version and a stand alone. Future versions will need a clap version and a linux version.

#extra features



Only do the following once everything else is done. dont even read this section if something is incomplete.

- a debug feature

in the help section, when the debug feature is invoked, it should open a window that displays all the raw data of the VST and show the data as features are invoked and settings are changed. There should also be a "create log file on crash" checkbox that, if checked, starts a log file and saves it as the new date and time it was invoked and is for debugging if the vst ever crashes. instruct the user where to find this debug log and how to send it to support if the vst continues to crash. this feature is turned off by default on every time the vst loads. this feature also includes any obvious signals of misconfiguration.

There should also be a "reset all settings to defautlt" button in the debug section which does a hard reset of the vst back to defaults and removes any cache files. This is a much more destructive reset than the 'reset' function in the gui and is used mainly for troubleshooting when the vst does not work. 

There should also be an "export troubleshooting file" which is different than the debug log that gets exported. when invoked the troubleshooting file does a light diagnostic on itself (the vst) and saves the results and all of the settings that the vst is currently set to, including the audio settings and midi settings, the license information, the version number, information about the DAW. When the 'create log file on crash' file is created a copy of the troubleshooting file will be included at the start of the file and is created when the user invokes the 'create log file on crash' feature.

The user should be instructed on what each of these files does and that if the vst is hard crashing then both need to be sent to support with a detailed discription of what the issue is. The troubleshooting file is meant to help the user as well as support figure out minor issues and not a full debug senario. the debug and troubleshooting features should be the very last thing that is done to the vst and should only be started once everything else is considered completed and has been throughly tested.


- easter egg

After everything is done create a hidden effect of your choice in the gui that will only show up when a spicific pixel in the gui is clicked. This will reveal a hidden tab with controlls for that effect. When the controlls are exposed there should be a close button that closes the hidden feature back up. the mouseover tool tips for this hidden effect should indicate that it is a secret hidden effect. 

# theme.md

impliment the custom theme.md file which should be included in the root of the folder.