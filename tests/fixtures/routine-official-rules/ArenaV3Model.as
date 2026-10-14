package mmo.arenav3.model
{
   import flash.events.EventDispatcher;
   import mmo.arenav3.config.ArenaV3BattleFieldConfig;
   import mmo.arenav3.config.ArenaV3Config;
   import mmo.arenav3.config.ArenaV3ShopConfig;
   import mmo.arenav3.defines.ArenaV3BattleFieldDefine;
   import mmo.arenav3.defines.ArenaV3ZoneDefine;
   import mmo.arenav3.event.ArenaV3Event;
   import mmo.arenav3.event.ArenaV3EventDispatcher;
   import mmo.common.shareobject.LocalStore;
   import mmo.common.user.UserInfo;
   import mmo.interfaces.IDispose;
   import mmo.interfaces.ServiceContainer;
   import mmo.interfaces.petcollection.IPetCollection;
   
   public class ArenaV3Model extends EventDispatcher implements IArenaV3Model, IDispose
   {
      
      private var _curFieldId:int;
      
      private var _curFieldInfos:Vector.<ArenaV3FieldInfo>;
      
      private var _joinedFieldIdAndZoneId:Array;
      
      private var _zoneUserNums:Array = [];
      
      private var _goodsData:Array;
      
      private var _useSystemStrengthenAttack:Array;
      
      private var _useSystemStrengthenDefend:Array;
      
      private var _formationData:Arenav3LuanDou_FormationData;
      
      private var _curFormationId:int;
      
      private var _curFormationCacheObj:Object;
      
      private var _remainSweepTime:int;
      
      public function ArenaV3Model()
      {
         super();
      }
      
      public function dispose() : void
      {
      }
      
      public function setArenaV3Info(param1:Object) : void
      {
         var _loc3_:int = 0;
         var _loc4_:Array = null;
         var _loc5_:Vector.<ArenaV3ZoneDefine> = null;
         var _loc6_:int = 0;
         var _loc7_:ArenaV3FieldInfo = null;
         if(LocalStore.getValue(ArenaV3Config.LS_KEY,UserInfo.userName) != null)
         {
            this._curFieldId = int(LocalStore.getValue(ArenaV3Config.LS_KEY,UserInfo.userName));
         }
         else
         {
            this._curFieldId = ArenaV3BattleFieldConfig.JING_DIAN_BATTLE_FIELD.id;
         }
         this._curFieldInfos = new Vector.<ArenaV3FieldInfo>();
         this._zoneUserNums = [];
         var _loc2_:int = 0;
         while(_loc2_ < ArenaV3BattleFieldConfig.JING_YING_BATTLE_FIELDS.length)
         {
            _loc3_ = _loc2_ + 1;
            if(param1["zao" + _loc3_])
            {
               _loc7_ = ArenaV3FieldInfo.parseInfo(ArenaV3BattleFieldConfig.JING_DIAN_BATTLE_FIELD.id + _loc2_,param1["zao" + _loc3_]);
               this._curFieldInfos.push(_loc7_);
            }
            _loc4_ = [];
            _loc5_ = ArenaV3BattleFieldConfig.JING_YING_BATTLE_FIELDS[_loc2_].zoneDefines;
            _loc6_ = 0;
            while(_loc6_ < _loc5_.length)
            {
               _loc4_.push(param1["un" + _loc5_[_loc6_].serverZoneId]);
               _loc6_++;
            }
            this._zoneUserNums.push(_loc4_);
            _loc2_++;
         }
         this._remainSweepTime = Math.max(0,ArenaV3Config.DAILY_SWEEP_TIME - param1["sweep"]);
         this._useSystemStrengthenAttack = [param1["pcoa1"],param1["pcoa2"]];
         this._useSystemStrengthenDefend = [param1["pcod1"],param1["pcod2"]];
      }
      
      public function setLuanDouCommonFormationInfo(param1:Object) : void
      {
         var serverResp:Object = param1;
         ServiceContainer.tryGetService(IPetCollection,function(param1:IPetCollection, param2:Object):void
         {
            _formationData = new Arenav3LuanDou_FormationData();
            updateFormation();
         });
      }
      
      public function updateLuanDouCommonFormation(param1:int, param2:Array) : void
      {
         var _loc3_:Array = this._curFormationCacheObj["formationList"];
         _loc3_[param1] = param2[0];
         this._curFormationCacheObj["formationList"] = _loc3_;
         this._formationData = new Arenav3LuanDou_FormationData();
         this.updateFormation();
      }
      
      public function updateCurFormationId(param1:int) : void
      {
         this._curFormationId = param1;
         this._curFormationCacheObj["curFormationId"] = this._curFormationId;
         this._curFormationCacheObj["curFormation"] = this._curFormationCacheObj["formationList"][this._curFormationId];
         this._formationData = new Arenav3LuanDou_FormationData();
         this.updateFormation();
      }
      
      public function updateFeildInfo(param1:Object) : void
      {
         var _loc2_:ArenaV3FieldInfo = null;
         if(param1["z"])
         {
            _loc2_ = ArenaV3FieldInfo.parseInfo(this._curFieldId,param1["z"]);
            this._curFieldInfos[this.curFieldIdx] = _loc2_;
         }
      }
      
      public function parseShopInfo(param1:Object) : void
      {
         var params:Object = param1;
         this._goodsData = ArenaV3ShopConfig.GOODS.map(function(param1:Object, ... rest):ArenaV3Goods
         {
            return new ArenaV3Goods(param1,params);
         }).sort(function(param1:ArenaV3Goods, param2:ArenaV3Goods):int
         {
            return param1.sortIndex - param2.sortIndex;
         });
      }
      
      public function replaceDefend(param1:int, param2:Boolean) : void
      {
         this.curFieldInfo.setDefendFormation();
         this._useSystemStrengthenDefend[param1] = param2;
      }
      
      public function shiftField(param1:int) : void
      {
         this._curFieldId = ArenaV3BattleFieldConfig.NEW_BATTLE_FIELD_DEFINES[param1].id;
      }
      
      public function useSystemStrengthen(param1:Boolean) : void
      {
         if(param1)
         {
            this._useSystemStrengthenAttack[this.curFieldIdx] = !this._useSystemStrengthenAttack[this.curFieldIdx];
         }
         else
         {
            this._useSystemStrengthenDefend[this.curFieldIdx] = !this._useSystemStrengthenDefend[this.curFieldIdx];
         }
      }
      
      public function updateFormation() : void
      {
         ArenaV3EventDispatcher.instance.dispatchEvent(new ArenaV3Event(ArenaV3Event.UPDATE_FORMATION));
      }
      
      public function gainRankOncePrize(param1:int) : void
      {
         var _loc2_:int = 0;
         while(_loc2_ < this._curFieldInfos.length)
         {
            this._curFieldInfos[_loc2_].afterGainOncePrize(param1);
            _loc2_++;
         }
      }
      
      public function sweep(param1:int, param2:Object) : void
      {
         this._curFieldInfos[param1].afterSweep(param2);
         --this._remainSweepTime;
      }
      
      public function getFieldInfoByIndex(param1:int) : ArenaV3FieldInfo
      {
         return this._curFieldInfos[param1];
      }
      
      public function get todayTotalChallengeTimes() : int
      {
         var _loc1_:int = 0;
         var _loc2_:int = 0;
         while(_loc2_ < this._curFieldInfos.length)
         {
            _loc1_ += this._curFieldInfos[_loc2_].myChallengeTime;
            _loc2_++;
         }
         return _loc1_;
      }
      
      public function get formData() : Arenav3LuanDou_FormationData
      {
         return this._formationData;
      }
      
      public function get goodsData() : Array
      {
         return this._goodsData;
      }
      
      public function get myCoin() : int
      {
         return ArenaV3Config.jingjiScoreNum;
      }
      
      public function get useSystemStrengthenAttack() : Boolean
      {
         return this._useSystemStrengthenAttack[this.curFieldIdx];
      }
      
      public function get useSystemStrengthenDefend() : Boolean
      {
         return this._useSystemStrengthenDefend[this.curFieldIdx];
      }
      
      public function get curFieldId() : int
      {
         return this._curFieldId;
      }
      
      public function get curFieldInfo() : ArenaV3FieldInfo
      {
         return this._curFieldInfos[this.curFieldIdx];
      }
      
      public function get jingjiScoreNum() : int
      {
         return ArenaV3Config.jingjiScoreNum;
      }
      
      public function get joinedFieldIdAndZoneId() : Array
      {
         return this._joinedFieldIdAndZoneId;
      }
      
      public function get needJoinManuallyWhenInJingying() : Boolean
      {
         if(ArenaV3BattleFieldConfig.getIsInJingyingField(this._curFieldId))
         {
            return this._joinedFieldIdAndZoneId.length < ArenaV3BattleFieldConfig.JING_YING_BATTLE_FIELDS.length;
         }
         return false;
      }
      
      public function get curFieldDefine() : ArenaV3BattleFieldDefine
      {
         return this.curFieldInfo.fieldDefine;
      }
      
      public function get curZoneDefine() : ArenaV3ZoneDefine
      {
         return this.curFieldDefine.getZoneDefineByServerId(this.curFieldInfo.curZoneId);
      }
      
      public function get zoneUserNums() : Array
      {
         return this._zoneUserNums;
      }
      
      public function get curFieldIdx() : int
      {
         var tmpFieldIndex:int = 0;
         ArenaV3BattleFieldConfig.NEW_BATTLE_FIELD_DEFINES.forEach(function(param1:ArenaV3BattleFieldDefine, param2:int, param3:*):*
         {
            if(param1.id == _curFieldId)
            {
               tmpFieldIndex = param2;
            }
         });
         return tmpFieldIndex;
      }
      
      public function get curFormationId() : int
      {
         return this._curFormationId;
      }
      
      public function get curFormationCacheObj() : Object
      {
         return this._curFormationCacheObj;
      }
      
      public function get remainSweepTime() : int
      {
         return this._remainSweepTime;
      }
   }
}

