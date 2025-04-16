# CardAction

This Windows application triggers events when a smart card is removed or inserted. You define the actions in a CardAction.ini file that looks like this:

```ini
[OnInsert]
APDUs=00A4040000,80CA9F7F00
Command=cmd.exe /c echo Card inserted {2} > %TEMP%\card_inserted.txt

[OnRemove]
Command=cmd.exe /c echo Card removed > %TEMP%\card_removed.txt
```

The above example will trigger the command when a card is inserted or removed. The command can be any executable, and you can also send APDU commands to be sent when the card is inserted. The responses from the APDUs can be used in the command using placeholders. In the example above `{2}` will be replaced with the response from the second APDU command. 