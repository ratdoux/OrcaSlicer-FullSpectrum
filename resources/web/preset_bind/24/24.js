function OnInit()
{
	//let strInput=JSON.stringify(cData);
	//HandleStudio(strInput);
	
	TranslatePage();
	
	RequestProfile();
}



function RequestProfile()
{
	var tSend={};
	tSend['sequence_id']=Math.round(new Date() / 1000);
	tSend['command']="request_userguide_profile";
	
	SendWXMessage( JSON.stringify(tSend) );
}

function HandleStudio( pVal )
{
//	alert(strInput);
//	alert(JSON.stringify(strInput));
//	
//	let pVal=IsJson(strInput);
//	if(pVal==null)
//	{
//		alert("Msg Format Error is not Json");
//		return;
//	}
	
	let strCmd=pVal['command'];
	//alert(strCmd);
	
	if(strCmd=='response_userguide_profile')
	{
		HandleModelList(pVal['response']);
	}
}

function ShowPrinterThumb(pItem, strImg)
{
	$(pItem).attr('src',strImg);
	$(pItem).attr('onerror',null);
}

function HandleModelList( pVal )
{
	if( !pVal.hasOwnProperty("model") )
		return;

    pModel=pVal['model'];
	
	let nTotal=pModel.length;
	let ModelHtml={};
	for(let n=0;n<nTotal;n++)
	{
		let OneModel=pModel[n];
		
		let strVendor=OneModel['vendor'];
		
		//Add Vendor Html Node
		if($(".OneVendorBlock[vendor='"+strVendor+"']").length==0)
		{
			let sVV=strVendor;
			if( sVV=="BBL" )
				sVV="Bambu Lab";			
			if( sVV=="Custom")
				sVV="Custom Printer";
			if( sVV=="Other")
				sVV="Orca colosseum";

			let HtmlNewVendor='<div class="OneVendorBlock" Vendor="'+strVendor+'">'+
'<div class="BlockBanner">'+
'	<a>'+sVV+'</a>'+
'</div>'+
'<div class="PrinterArea">	'+
'</div>'+
'</div>';
			
			$('#Content').append(HtmlNewVendor);
		}
		
		let ModelName=OneModel['model'];
		
		//Collect Html Node Nozzel Html
		if( !ModelHtml.hasOwnProperty(strVendor))
			ModelHtml[strVendor]='';
			
		let HtmlNozzel = '<div class="pNozzel TextS2">' +
			'<input type="checkbox" ' +
			'model="' + OneModel['model'] + '" ' +
			'vendor="' + strVendor + '" ' +
			'nozzle_all="' + OneModel['nozzle_diameter'] + '" ' +
			'onclick="CheckBoxOnclick(this)" /></div>';
		
		let CoverImage=OneModel['cover'];
		ModelHtml[strVendor]+='<div class="PrinterBlock">'+
'	<div class="PImg"><img src="'+CoverImage+'"  /></div>'+
'    <div class="PNameRow">'+ HtmlNozzel +'<div class="PName">'+OneModel['model']+'</div></div></div>';
	}
	
	//Update Nozzel Html Append
	for( let key in ModelHtml )
	{
		$(".OneVendorBlock[vendor='"+key+"'] .PrinterArea").append( ModelHtml[key] );
	}
	
	
	//Update Checkbox
	$('input').prop("checked", false);
	for(let m=0;m<nTotal;m++)
	{
		let OneModel=pModel[m];

		let SelectList=OneModel['nozzle_selected'];
		let checked = SelectList != '';
		$("input[vendor='" + OneModel['vendor'] + "'][model='" + OneModel['model'] + "']").prop("checked", checked);
		let allNozzles = OneModel['nozzle_diameter'].split(';');
		for (let a = 0; a < allNozzles.length; a++) {
			let nNozzel = allNozzles[a];
			if (nNozzel != '')
				SetModelSelect(OneModel['vendor'], OneModel['model'], nNozzel, checked);
		}
	}	

	// let AlreadySelect=$("input:checked");
	// let nSelect=AlreadySelect.length;
	// if(nSelect==0)
	// {
	// 	$("input[nozzel='0.4'][vendor='Custom']").prop("checked", true);
	// }
	
	TranslatePage();
}

function CheckBoxOnclick(obj) {
    // 多选页面：按机型勾选，内部映射到该机型全部喷嘴。
    let strModel = obj.getAttribute("model");
    let strVendor = obj.getAttribute("vendor");
    let nozzles = (obj.getAttribute("nozzle_all") || "").split(";");

    if (!ModelNozzleSelected.hasOwnProperty(strVendor))
        ModelNozzleSelected[strVendor] = {};
    if (!ModelNozzleSelected[strVendor].hasOwnProperty(strModel))
        ModelNozzleSelected[strVendor][strModel] = {};

    for (let i = 0; i < nozzles.length; i++) {
        if (nozzles[i] != '')
            ModelNozzleSelected[strVendor][strModel][nozzles[i]] = obj.checked;
    }
}

function SetModelSelect(vendor, model, nozzel, checked) {
	if (!ModelNozzleSelected.hasOwnProperty(vendor) && !checked) {
		return;
	}

	if (!ModelNozzleSelected.hasOwnProperty(vendor) && checked) {
		ModelNozzleSelected[vendor] = {};
	}

	let oVendor = ModelNozzleSelected[vendor];
	if (!oVendor.hasOwnProperty(model)) {
		oVendor[model] = {};
	}

	let oModel = oVendor[model];
	if (oModel.hasOwnProperty(nozzel) || checked) {
		oVendor[model][nozzel] = checked;
	}
}

function GetModelSelect(vendor, model, nozzel) {
	if (!ModelNozzleSelected.hasOwnProperty(vendor)) {
		return false;
	}

	let oVendor = ModelNozzleSelected[vendor];
	if (!oVendor.hasOwnProperty(model)) {
		return false;
	}

	let oModel = oVendor[model];
	if (!oModel.hasOwnProperty(nozzel)) {
		return false;
	}

	return oVendor[model][nozzel];
}

function FilterModelList(keyword) {
    // 移除保存checkbox状态的代码
    /*
    let ModelSelect = $('input[type=checkbox]');
    for (let n = 0; n < ModelSelect.length; n++) {
        let OneItem = ModelSelect[n];
        let strModel = OneItem.getAttribute("model");
        let strVendor = OneItem.getAttribute("vendor");
        let strNozzel = OneItem.getAttribute("nozzel");
        SetModelSelect(strVendor, strModel, strNozzel, OneItem.checked);
    }
    */

    let nTotal = pModel.length;
    let ModelHtml = {};
    
    $('#Content').empty();
    for (let n = 0; n < nTotal; n++) {
        let OneModel = pModel[n];

        let strVendor = OneModel['vendor'];
        let ModelName = OneModel['model'];
        if (ModelName.toLowerCase().indexOf(keyword.toLowerCase()) == -1)
            continue;

        //Add Vendor Html Node
        if ($(".OneVendorBlock[vendor='" + strVendor + "']").length == 0) {
            let sVV = strVendor;
            if (sVV == "BBL")
                sVV = "Bambu Lab";
            if (sVV == "Custom")
                sVV = "Custom Printer";
            if (sVV == "Other")
                sVV = "Orca colosseum";

            let HtmlNewVendor = '<div class="OneVendorBlock" Vendor="' + strVendor + '">' +
                '<div class="BlockBanner">' +
                '	<a>' + sVV + '</a>' +
                '</div>' +
                '<div class="PrinterArea">	' +
                '</div>' +
                '</div>';

            $('#Content').append(HtmlNewVendor);
        }

        //Collect Html Node Nozzel Html
        if (!ModelHtml.hasOwnProperty(strVendor))
            ModelHtml[strVendor] = '';

        let HtmlNozzel = '<div class="pNozzel TextS2">' +
            '<input type="checkbox" ' +
            'model="' + OneModel['model'] + '" ' +
            'vendor="' + strVendor + '" ' +
            'nozzle_all="' + OneModel['nozzle_diameter'] + '" ' +
            'onclick="CheckBoxOnclick(this)" /></div>';

        let CoverImage = OneModel['cover'];
        ModelHtml[strVendor] += '<div class="PrinterBlock">' +
            '	<div class="PImg"><img src="' + CoverImage + '"  /></div>' +
            '    <div class="PNameRow">' + HtmlNozzel + '<div class="PName">' + OneModel['model'] + '</div></div></div>';
    }

    //Update Nozzel Html Append
    for (let key in ModelHtml) {
        let obj = $(".OneVendorBlock[vendor='" + key + "'] .PrinterArea");
        obj.empty();
        obj.append(ModelHtml[key]);
    }

    // 更新radio选中状态
    $('input[type="checkbox"]').each(function() {
        let strModel = $(this).attr("model");
        let strVendor = $(this).attr("vendor");
        let nozzles = (($(this).attr("nozzle_all")) || "").split(';');
        let checked = false;
        for (let i = 0; i < nozzles.length; i++) {
            let strNozzel = nozzles[i];
            if (strNozzel != '' && ModelNozzleSelected[strVendor] &&
                ModelNozzleSelected[strVendor][strModel] &&
                ModelNozzleSelected[strVendor][strModel][strNozzel]) {
                checked = true;
                break;
            }
        }
        if (checked) $(this).prop("checked", true);
    });

    TranslatePage();
}

function OnExitFilter() {

	let nTotal = 0;
	let ModelAll = {};
	for (vendor in ModelNozzleSelected) {
		for (model in ModelNozzleSelected[vendor]) {
			let anyChecked = false;
			for (nozzel in ModelNozzleSelected[vendor][model]) {
				if (ModelNozzleSelected[vendor][model][nozzel]) {
					anyChecked = true;
					break;
				}
			}
			if (!anyChecked)
				continue;

			for (let i = 0; i < pModel.length; i++) {
				let pm = pModel[i];
				if (pm['vendor'] == vendor && pm['model'] == model) {
					ModelAll[model] = {};
					ModelAll[model]["model"] = model;
					ModelAll[model]["nozzle_diameter"] = pm["nozzle_diameter"];
					ModelAll[model]["vendor"] = vendor;
					nTotal++;
					break;
				}
			}
		}
	}

	var tSend = {};
	tSend['sequence_id'] = Math.round(new Date() / 1000);
	tSend['command'] = "save_userguide_models";
	tSend['data'] = ModelAll;

	SendWXMessage(JSON.stringify(tSend));

	return nTotal;

}

//
function OnExit()
{	
	let ModelAll={};
	
	let ModelSelect=$("input:checked");
	let nTotal=ModelSelect.length;

	if( nTotal==0 )
	{
		ShowNotice(1);
		
		return 0;
	}
	
	for(let n=0;n<nTotal;n++)
	{
	    let OneItem=ModelSelect[n];
		
		let strModel=OneItem.getAttribute("model");
		let strVendor=OneItem.getAttribute("vendor");
		let strNozzelAll=OneItem.getAttribute("nozzle_all");
			
		//alert(strModel+strVendor+strNozzel);
		
		if(!ModelAll.hasOwnProperty(strModel))
		{
			//alert("ADD: "+strModel);
			
			ModelAll[strModel]={};
		
			ModelAll[strModel]["model"]=strModel;
			ModelAll[strModel]["nozzle_diameter"]='';
			ModelAll[strModel]["vendor"]=strVendor;
		}
		
		ModelAll[strModel]["nozzle_diameter"]=strNozzelAll;
	}
		
	var tSend={};
	tSend['sequence_id']=Math.round(new Date() / 1000);
	tSend['command']="save_userguide_models";
	tSend['data']=ModelAll;
	
	SendWXMessage( JSON.stringify(tSend) );

    return nTotal;
}


function ShowNotice( nShow )
{
	if(nShow==0)
	{
		$("#NoticeMask").hide();
		$("#NoticeBody").hide();
	}
	else
	{
		$("#NoticeMask").show();
		$("#NoticeBody").show();
	}
}

function ConfirmSelect()
{
	let nChoose=OnExitFilter();
	
	if(nChoose>0)
    {
		var tSend={};
		tSend['sequence_id']=Math.round(new Date() / 1000);
		tSend['command']="user_guide_finish";
		tSend['data']={};
		tSend['data']['action']="finish";
		
		SendWXMessage( JSON.stringify(tSend) );			
	}
}




